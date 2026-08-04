#include "aegra/apps/worker/windows_personal_backup.h"

#include "windows_personal_backup_runtime.h"
#include "worker_task_log.h"

#include "aegra/contracts/service_control.h"
#include "aegra/format/manifest.h"

#include <algorithm>
#include <sstream>
#include <set>
#include <string>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::ranges::all_of(value, [](const std::byte item) { return item == std::byte{0}; });
}

base::Result<void> validate_request(const WindowsPersonalBackupRequest& request) {
    if (request.job_id.empty() || request.trace_id.empty() || request.volume_guid_paths.empty() ||
        request.volume_guid_paths.size() > contracts::kMaximumBackupSources ||
        request.destination.empty() || request.password.empty() || request.created_utc.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup request is incomplete",
        });
    }
    std::set<std::filesystem::path> unique_sources;
    for (const auto& source : request.volume_guid_paths) {
        if (source.empty() || !unique_sources.insert(source).second) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "Windows personal backup sources are invalid",
            });
        }
    }
    if (request.block_size_bytes == 0 || request.chunk_size_bytes < request.block_size_bytes ||
        request.memory_budget_bytes < request.chunk_size_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup geometry is invalid",
        });
    }
    const bool full = request.backup_type == WindowsPersonalBackupType::kFull;
    const bool incremental = request.backup_type == WindowsPersonalBackupType::kIncremental;
    const bool parent_empty = request.parent_source.empty() && request.parent_password.empty();
    const bool parent_complete = !request.parent_source.empty() && !request.parent_password.empty();
    if ((!full && !incremental) || (full && !parent_empty) ||
        (incremental && !parent_complete)) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup relationship is invalid",
        });
    }
    if (is_zero_uuid(request.file_uuid) ||
        (full && (is_zero_uuid(request.backup_set_uuid) ||
                  request.file_uuid == request.backup_set_uuid)) ||
        (incremental && !is_zero_uuid(request.backup_set_uuid))) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup identifiers are invalid",
        });
    }
    return base::Result<void>::success();
}

std::string utf8_path(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string result;
    result.reserve(encoded.size());
    for (const auto value : encoded) {
        result.push_back(static_cast<char>(value));
    }
    return result;
}

format::Manifest make_manifest(const WindowsPersonalBackupRequest& request,
                               const std::vector<PreparedVolumeMetadata>& sources) {
    format::Manifest manifest;
    manifest.system.hostname = request.hostname;
    manifest.system.collection_time_utc = request.created_utc;
    manifest.backup_job.backup_type =
        request.backup_type == WindowsPersonalBackupType::kFull
            ? format::BackupType::kFull
            : format::BackupType::kIncremental;
    manifest.backup_job.created_utc = request.created_utc;
    manifest.backup_job.application_version = request.application_version;

    for (std::size_t index = 0; index < sources.size(); ++index) {
        const auto& metadata = sources[index];
        format::Volume volume;
        volume.volume_index = static_cast<std::uint32_t>(index);
        volume.volume_id = utf8_path(metadata.volume_guid_path);
        volume.volume_guid = volume.volume_id;
        volume.filesystem = metadata.filesystem;
        volume.label = metadata.label;
        volume.total_size = metadata.logical_size_bytes;
        volume.cluster_size = metadata.cluster_size_bytes;
        volume.vss_required = metadata.vss_used;
        volume.vss_used = metadata.vss_used;
        volume.consistency_level = metadata.vss_used ? format::ConsistencyLevel::kApplication
                                                     : format::ConsistencyLevel::kCrash;
        for (const auto& mount_point : metadata.mount_points) {
            volume.mount_points.push_back(utf8_path(mount_point));
        }
        manifest.volumes.push_back(std::move(volume));
    }
    return manifest;
}

std::optional<base::Error> release_snapshot(PreparedVolumeSources& prepared) {
    prepared.sources.clear();
    if (!prepared.snapshot) {
        return std::nullopt;
    }
    auto closed = prepared.snapshot->close();
    if (!closed) {
        return closed.error();
    }
    return std::nullopt;
}

} // namespace

void log_runtime_info(const std::string& message) {
    if (auto* log = WorkerTaskLog::active()) {
        log->info(message);
    }
}

base::Result<pipeline::BackupSummary>
run_volume_pipelines(const WindowsPersonalBackupRequest& request, PreparedVolumeSources& prepared,
                     ports::IBackupSession& session, ports::IProgressSink* progress,
                     const base::CancellationToken& cancellation) {
    pipeline::BackupSummary total;
    for (std::size_t index = 0; index < prepared.sources.size(); ++index) {
        const auto& metadata = prepared.metadata[index];
        std::ostringstream line;
        line << "Backing up volume " << utf8_path(metadata.volume_guid_path) << " size "
             << metadata.logical_size_bytes << " bytes mode "
             << (metadata.vss_used ? "vss" : "raw");
        log_runtime_info(line.str());
        pipeline::BackupPlan plan{request.job_id, request.trace_id, request.chunk_size_bytes,
                                  request.memory_budget_bytes};
        plan.source_index = static_cast<std::uint32_t>(index);
        plan.commit_mode = index + 1 == prepared.sources.size()
                               ? pipeline::BackupCommitMode::kCommit
                               : pipeline::BackupCommitMode::kDefer;
        pipeline::BackupPipeline pipeline(*prepared.sources[index], session, progress);
        auto backup = pipeline.run(plan, cancellation);
        if (!backup) {
            return base::Result<pipeline::BackupSummary>::failure(backup.error());
        }
        total.logical_bytes += backup.value().logical_bytes;
        total.stored_bytes += backup.value().stored_bytes;
        total.chunk_count += backup.value().chunk_count;
        total.peak_buffered_bytes =
            (std::max)(total.peak_buffered_bytes, backup.value().peak_buffered_bytes);
    }
    return base::Result<pipeline::BackupSummary>::success(total);
}

base::Result<WindowsPersonalBackupResult> backup_windows_personal_volumes_with_runtime(
    const WindowsPersonalBackupRequest& request, const base::CancellationToken& cancellation,
    ports::IProgressSink* progress, IWindowsPersonalBackupRuntime& runtime) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<WindowsPersonalBackupResult>::failure(validation.error());
    }
    auto prepared = runtime.prepare_sources(request.volume_guid_paths, cancellation);
    if (!prepared) {
        return base::Result<WindowsPersonalBackupResult>::failure(prepared.error());
    }

    auto manifest = make_manifest(request, prepared.value().metadata);
    auto session = runtime.create_archive(request, manifest);
    if (!session) {
        (void)release_snapshot(prepared.value());
        return base::Result<WindowsPersonalBackupResult>::failure(session.error());
    }
    log_runtime_info(std::string("Archive destination: ") + utf8_path(request.destination));
    log_runtime_info("Prepared VSS and raw volume sources; starting backup pipelines");

    auto backup = run_volume_pipelines(request, prepared.value(), *session.value(), progress,
                                       cancellation);
    auto cleanup_error = release_snapshot(prepared.value());
    if (!backup) {
        return base::Result<WindowsPersonalBackupResult>::failure(backup.error());
    }
    if (cleanup_error) {
        log_runtime_info("Snapshot cleanup reported a non-fatal error after commit");
    }
    return base::Result<WindowsPersonalBackupResult>::success(
        WindowsPersonalBackupResult{backup.value(), std::move(cleanup_error)});
}

} // namespace detail

base::Result<WindowsPersonalBackupResult>
backup_windows_personal_volumes(const WindowsPersonalBackupRequest& request,
                                const base::CancellationToken& cancellation,
                                ports::IProgressSink* progress) {
    try {
        auto runtime = detail::make_windows_personal_backup_runtime();
        return detail::backup_windows_personal_volumes_with_runtime(request, cancellation, progress,
                                                                    *runtime);
    } catch (...) {
        return base::Result<WindowsPersonalBackupResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows personal volume backup failed unexpectedly",
        });
    }
}

} // namespace aegra::apps::worker
