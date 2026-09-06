#include "mount_host_session.h"

#include "aegra/adapters/dokan/disk_mount.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/apps/worker/worker_task_log.h"
#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace {

using Json = nlohmann::json;

[[nodiscard]] base::Error make_error(const base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string_view value) {
    const auto* begin = reinterpret_cast<const char8_t*>(value.data());
    return std::filesystem::path(std::u8string(begin, begin + value.size()));
}

[[nodiscard]] std::string encode_event(const std::string_view kind,
                                       const std::string_view session_id,
                                       const std::string_view message_code,
                                       const adapters::dokan::MountSessionInfo* info) {
    Json payload{{"schema_version", 1},
                 {"kind", std::string(kind)},
                 {"session_id", std::string(session_id)},
                 {"message_code", std::string(message_code)}};
    if (info != nullptr) {
        payload["mount_point"] = info->mount_point;
        payload["drive_letters"] = info->drive_letters;
        payload["device_number"] = info->device_number;
        payload["disk_size_bytes"] = info->disk_size_bytes;
        payload["source_disk_number"] = info->source_disk_number;
    } else {
        payload["mount_point"] = "";
        payload["drive_letters"] = Json::array();
        payload["device_number"] = 0xFFFFFFFF;
        payload["disk_size_bytes"] = 0;
        payload["source_disk_number"] = 0;
    }
    return payload.dump();
}

struct MountJob final {
    std::string session_id;
    std::string content_kind;
    std::uint32_t source_disk_number{0};
    std::string preferred_drive_letter;
    std::filesystem::path overlay_dir;
    std::vector<adapters::personal_archive::ArchiveOpenRequest> layers;
    // Keep password storage alive for ArchiveOpenRequest string_view fields.
    std::vector<std::string> layer_paths;
    std::vector<std::string> layer_passwords;
};

[[nodiscard]] base::Result<MountJob> parse_mount_job(const std::string& frame) {
    Json root;
    try {
        root = Json::parse(frame);
    } catch (const std::exception&) {
        return base::Result<MountJob>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "mount host request is not valid JSON"));
    }
    if (!root.is_object() || root.value("schema_version", 0) != 1 ||
        root.value("operation", "") != "mount") {
        return base::Result<MountJob>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "mount host request schema is invalid"));
    }
    MountJob job;
    job.session_id = root.value("session_id", "");
    job.content_kind = root.value("content_kind", "volume_set");
    job.source_disk_number = root.value("source_disk_number", 0U);
    job.preferred_drive_letter = root.value("preferred_drive_letter", "");
    const auto overlay = root.value("overlay_dir", "");
    if (job.session_id.empty() || overlay.empty() || !root.contains("layers") ||
        !root.at("layers").is_array() || root.at("layers").empty()) {
        return base::Result<MountJob>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "mount host request fields are invalid"));
    }
    job.overlay_dir = path_from_utf8(overlay);
    job.layer_paths.reserve(root.at("layers").size());
    job.layer_passwords.reserve(root.at("layers").size());
    job.layers.reserve(root.at("layers").size());
    for (const auto& layer : root.at("layers")) {
        if (!layer.is_object()) {
            return base::Result<MountJob>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "mount host layer is invalid"));
        }
        job.layer_paths.push_back(layer.value("path", ""));
        job.layer_passwords.push_back(layer.value("password", ""));
        if (job.layer_paths.back().empty()) {
            return base::Result<MountJob>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "mount host layer path is empty"));
        }
        adapters::personal_archive::ArchiveOpenRequest open_request;
        open_request.source = path_from_utf8(job.layer_paths.back());
        open_request.password = job.layer_passwords.back();
        job.layers.push_back(std::move(open_request));
    }
    return base::Result<MountJob>::success(std::move(job));
}

[[nodiscard]] bool is_unmount_command(const std::string& frame, const std::string_view session_id) {
    try {
        const auto root = Json::parse(frame);
        return root.is_object() && root.value("schema_version", 0) == 1 &&
               root.value("operation", "") == "unmount" &&
               root.value("session_id", "") == session_id;
    } catch (const std::exception&) {
        return false;
    }
}

[[nodiscard]] base::Result<void> send_frame(ports::IMessageChannel& channel, std::string frame,
                                            const base::CancellationToken cancellation) {
    return channel.send(frame, cancellation);
}

struct MountedResources final {
    adapters::dokan::MountSessionInfo info;
    std::unique_ptr<adapters::personal_archive::PersonalArchiveChainReader> chain;
    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveChainReader> file_chain;
    std::unique_ptr<adapters::personal_archive::WholeDiskByteReader> disk_reader;
};

void log_mount_request(WorkerTaskLog* log, const MountJob& job) {
    if (log == nullptr) {
        return;
    }
    const bool password_present =
        std::ranges::any_of(job.layer_passwords, [](const auto& value) { return !value.empty(); });
    log->section("Request");
    log->field("session_id", job.session_id);
    log->field("content_kind", job.content_kind);
    log->field_u64("source_disk_number", job.source_disk_number);
    log->field("preferred_drive_letter",
               job.preferred_drive_letter.empty() ? "automatic" : job.preferred_drive_letter);
    log->field_u64("archive_layers", job.layers.size());
    log->field("password", password_present ? "present" : "empty");
}

void log_mount_result(WorkerTaskLog* log, const std::string_view status,
                      const std::string_view message_code,
                      const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    log->field("status", status);
    log->field("message_code", message_code);
    log->field("elapsed", format_duration_ms(elapsed));
}

[[nodiscard]] std::string stable_mount_message_code(const base::Error& error) {
    constexpr std::array<std::string_view, 10> kCodes{
        "mount.invalid_argument", "mount.dokan_unavailable",    "mount.disk_not_found",
        "mount.already_mounted",  "mount.no_data_partition",    "mount.dokan_failed",
        "mount.attach_failed",    "mount.no_free_drive_letter", "mount.unmount_failed",
        "mount.host_failed"};
    for (const auto code : kCodes) {
        if (error.message == code ||
            (error.message.starts_with(code) && error.message.size() > code.size() &&
             error.message[code.size()] == ':')) {
            return std::string(code);
        }
    }
    return "mount.host_failed";
}

[[nodiscard]] base::Result<MountedResources>
mount_file_set(MountJob& job,
               const adapters::personal_archive::ArchiveChainOpenRequest& open_request,
               WorkerTaskLog* log) {
    MountedResources resources;
    {
        ScopedStage stage(log, "open_file_archive_chain");
        auto opened =
            adapters::personal_archive::PersonalFileArchiveChainReader::open(open_request);
        if (!opened) {
            stage.fail(opened.error(), "open_chain");
            return base::Result<MountedResources>::failure(opened.error());
        }
        resources.file_chain = std::move(opened).value();
        stage.note_u64("layers", open_request.layers.size());
    }
    {
        ScopedStage stage(log, "mount_file_namespace");
        auto mounted = adapters::dokan::mount_file_set_readonly(
            *resources.file_chain, job.preferred_drive_letter, job.session_id);
        if (!mounted) {
            stage.fail(mounted.error(), "dokan_mount");
            return base::Result<MountedResources>::failure(mounted.error());
        }
        resources.info = std::move(mounted).value();
        stage.note("mount_point", resources.info.mount_point);
    }
    return base::Result<MountedResources>::success(std::move(resources));
}

[[nodiscard]] base::Result<MountedResources>
mount_volume_set(MountJob& job,
                 const adapters::personal_archive::ArchiveChainOpenRequest& open_request,
                 WorkerTaskLog* log) {
    MountedResources resources;
    {
        ScopedStage stage(log, "open_archive_chain");
        auto opened = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
        if (!opened) {
            stage.fail(opened.error(), "open_chain");
            return base::Result<MountedResources>::failure(opened.error());
        }
        resources.chain = std::move(opened).value();
        stage.note_u64("layers", open_request.layers.size());
    }
    {
        ScopedStage stage(log, "open_whole_disk_reader");
        // Re-identify the presented disk (fresh MBR signature + GPT DiskGUID) so a
        // read-only attach does not collide with a still-online source disk and get
        // forced OFFLINE by Windows.
        auto disk = adapters::personal_archive::WholeDiskByteReader::open(
            *resources.chain, resources.chain->manifest(), job.source_disk_number,
            /*cache_chunk_count=*/8, /*assign_unique_disk_identity=*/true);
        if (!disk) {
            stage.fail(disk.error(), "open_disk");
            if (disk.error().code == base::ErrorCode::kNotFound) {
                std::string message = "mount.disk_not_found";
                if (!disk.error().message.empty()) {
                    message.append(": ");
                    message.append(disk.error().message);
                }
                return base::Result<MountedResources>::failure(
                    make_error(disk.error().code, std::move(message)));
            }
            return base::Result<MountedResources>::failure(disk.error());
        }
        resources.disk_reader = std::move(disk).value();
        // Version probe: present only in the build that re-identifies the disk.
        stage.note_bool("reidentified_disk", true);
        for (const auto& manifest_disk : resources.chain->manifest().disks) {
            if (manifest_disk.disk_number != job.source_disk_number) {
                continue;
            }
            const char* style = manifest_disk.partition_style == format::PartitionStyle::kGpt
                                    ? "gpt"
                                    : (manifest_disk.partition_style == format::PartitionStyle::kMbr
                                           ? "mbr"
                                           : "raw");
            stage.note("partition_style", style);
            stage.note_u64("mbr_sector_bytes", manifest_disk.raw_layout.mbr_sector.size());
            stage.note_u64("gpt_primary_header_bytes",
                           manifest_disk.raw_layout.gpt_primary_header.size());
            stage.note_u64("gpt_backup_header_bytes",
                           manifest_disk.raw_layout.gpt_backup_header.size());
            break;
        }
    }
    {
        ScopedStage stage(log, "present_and_attach_virtual_disk");
        auto mounted = adapters::dokan::mount_whole_disk_readonly(
            *resources.disk_reader, resources.chain->manifest(), job.source_disk_number,
            job.preferred_drive_letter, job.overlay_dir, job.session_id);
        if (!mounted) {
            stage.fail(mounted.error(), "attach_vhdx",
                       "Check the Win32 code and Dokan/Virtual Disk availability");
            return base::Result<MountedResources>::failure(mounted.error());
        }
        resources.info = std::move(mounted).value();
        stage.note("mount_point", resources.info.mount_point);
        stage.note_u64("windows_disk_number", resources.info.device_number);
        stage.note_bytes("mounted_data_size", resources.info.disk_size_bytes);
    }
    return base::Result<MountedResources>::success(std::move(resources));
}

} // namespace

base::Result<void> run_mount_host_session(ports::IMessageChannel& channel,
                                          const base::CancellationToken cancellation) {
    auto request = channel.receive(cancellation);
    if (!request) {
        return base::Result<void>::failure(request.error());
    }
    auto job = parse_mount_job(request.value());
    if (!job) {
        (void)send_frame(channel, encode_event("failed", "", "mount.invalid_argument", nullptr),
                         cancellation);
        return base::Result<void>::failure(job.error());
    }

    const auto started = std::chrono::steady_clock::now();
    auto task_log = WorkerTaskLog::open("mount", job.value().session_id);
    WorkerTaskLogScope log_scope(task_log.get());
    log_mount_request(task_log.get(), job.value());
    {
        ScopedStage stage(task_log.get(), "check_dokan");
        if (!adapters::dokan::is_dokan_available()) {
            const auto error = make_error(base::ErrorCode::kConflict, "mount.dokan_unavailable");
            stage.fail(error, "driver_probe");
            log_mount_result(task_log.get(), "failed", error.message, started);
            (void)send_frame(channel,
                             encode_event("failed", job.value().session_id, error.message, nullptr),
                             cancellation);
            return base::Result<void>::failure(error);
        }
    }

    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.layers = std::move(job.value().layers);
    auto mounted = job.value().content_kind == "file_set"
                       ? mount_file_set(job.value(), open_request, task_log.get())
                       : mount_volume_set(job.value(), open_request, task_log.get());
    if (!mounted) {
        const auto code = stable_mount_message_code(mounted.error());
        log_mount_result(task_log.get(), "failed", code, started);
        (void)send_frame(channel, encode_event("failed", job.value().session_id, code, nullptr),
                         cancellation);
        return base::Result<void>::failure(mounted.error());
    }

    auto announced = send_frame(channel,
                                encode_event("mounted", job.value().session_id,
                                             "mount.session_mounted", &mounted.value().info),
                                cancellation);
    if (!announced) {
        (void)adapters::dokan::unmount_session(job.value().session_id);
        log_mount_result(task_log.get(), "failed", "mount.host_failed", started);
        return announced;
    }

    // Hold the session until Service sends unmount or the pipe disconnects.
    {
        ScopedStage stage(task_log.get(), "mounted_session");
        while (!cancellation.stop_requested()) {
            auto next = channel.receive(cancellation);
            if (!next) {
                stage.note("end_reason", "service_pipe_closed");
                break;
            }
            if (is_unmount_command(next.value(), job.value().session_id)) {
                stage.note("end_reason", "unmount_requested");
                break;
            }
        }
    }

    const auto unmounted = [&]() {
        ScopedStage cleanup_stage(task_log.get(), "unmount_and_cleanup");
        auto result = adapters::dokan::unmount_session(job.value().session_id);
        if (!result) {
            cleanup_stage.fail(result.error(), "unmount_session");
        }
        return result;
    }();
    (void)send_frame(channel,
                     encode_event("unmounted", job.value().session_id,
                                  unmounted ? "mount.unmounted" : "mount.unmount_failed", nullptr),
                     cancellation);
    log_mount_result(task_log.get(), unmounted ? "succeeded" : "failed",
                     unmounted ? "mount.unmounted" : "mount.unmount_failed", started);
    return unmounted ? base::Result<void>::success()
                     : base::Result<void>::failure(unmounted.error());
}

} // namespace aegra::apps::worker
