#include "mount_host_session.h"

#include "aegra/adapters/dokan/disk_mount.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

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

[[nodiscard]] std::string encode_event(const std::string_view kind, const std::string_view session_id,
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

} // namespace

base::Result<void> run_mount_host_session(ports::IMessageChannel& channel,
                                          const base::CancellationToken cancellation) {
    if (!adapters::dokan::is_dokan_available()) {
        auto sent = send_frame(channel,
                               encode_event("failed", "", "mount.dokan_unavailable", nullptr),
                               cancellation);
        if (!sent) {
            return sent;
        }
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kConflict, "mount.dokan_unavailable"));
    }

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

    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.layers = std::move(job.value().layers);
    auto chain = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
    if (!chain) {
        (void)send_frame(
            channel,
            encode_event("failed", job.value().session_id, "mount.host_failed", nullptr),
            cancellation);
        return base::Result<void>::failure(chain.error());
    }

    auto disk_reader = adapters::personal_archive::WholeDiskByteReader::open(
        *chain.value(), chain.value()->manifest(), job.value().source_disk_number);
    if (!disk_reader) {
        const auto code = disk_reader.error().code == base::ErrorCode::kNotFound
                              ? "mount.disk_not_found"
                              : "mount.host_failed";
        (void)send_frame(channel, encode_event("failed", job.value().session_id, code, nullptr),
                         cancellation);
        return base::Result<void>::failure(disk_reader.error());
    }

    auto mounted = adapters::dokan::mount_whole_disk_readonly(
        *disk_reader.value(), chain.value()->manifest(), job.value().source_disk_number,
        job.value().preferred_drive_letter, job.value().overlay_dir, job.value().session_id);
    if (!mounted) {
        const auto code = mounted.error().message.empty() ? "mount.host_failed"
                                                          : mounted.error().message;
        (void)send_frame(channel, encode_event("failed", job.value().session_id, code, nullptr),
                         cancellation);
        return base::Result<void>::failure(mounted.error());
    }

    auto announced =
        send_frame(channel,
                   encode_event("mounted", job.value().session_id, "mount.session_mounted",
                                &mounted.value()),
                   cancellation);
    if (!announced) {
        (void)adapters::dokan::unmount_session(job.value().session_id);
        return announced;
    }

    // Hold the session until Service sends unmount or the pipe disconnects.
    while (!cancellation.stop_requested()) {
        auto next = channel.receive(cancellation);
        if (!next) {
            break;
        }
        if (is_unmount_command(next.value(), job.value().session_id)) {
            break;
        }
    }

    auto unmounted = adapters::dokan::unmount_session(job.value().session_id);
    (void)send_frame(channel,
                     encode_event("unmounted", job.value().session_id,
                                  unmounted ? "mount.unmounted" : "mount.unmount_failed", nullptr),
                     cancellation);
    // Keep reader/chain alive until unmount completes (already done above).
    disk_reader.value().reset();
    chain.value().reset();
    return unmounted ? base::Result<void>::success()
                     : base::Result<void>::failure(unmounted.error());
}

} // namespace aegra::apps::worker
