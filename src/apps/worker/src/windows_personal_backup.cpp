#include "aegra/apps/worker/windows_personal_backup.h"

#include "windows_personal_backup_runtime.h"

#include "aegra/format/manifest.h"

#include <algorithm>
#include <string>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::ranges::all_of(value, [](const std::byte item) { return item == std::byte{0}; });
}

base::Result<void> validate_request(const WindowsPersonalVolumeBackupRequest& request) {
    if (request.job_id.empty() || request.volume_guid_path.empty() || request.destination.empty() ||
        request.password.empty() || request.created_utc.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup request is incomplete",
        });
    }
    if (request.block_size_bytes == 0 || request.chunk_size_bytes < request.block_size_bytes ||
        request.memory_budget_bytes < request.chunk_size_bytes) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "Windows personal volume backup geometry is invalid",
        });
    }
    if (is_zero_uuid(request.file_uuid) || is_zero_uuid(request.backup_set_uuid) ||
        request.file_uuid == request.backup_set_uuid) {
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

format::Manifest make_manifest(const WindowsPersonalVolumeBackupRequest& request,
                               const PreparedVolumeMetadata& metadata) {
    format::Manifest manifest;
    manifest.system.hostname = request.hostname;
    manifest.system.collection_time_utc = request.created_utc;
    manifest.backup_job.backup_type = format::BackupType::kFull;
    manifest.backup_job.created_utc = request.created_utc;
    manifest.backup_job.application_version = request.application_version;

    format::Volume volume;
    volume.volume_index = 0;
    volume.volume_id = utf8_path(metadata.volume_guid_path);
    volume.volume_guid = volume.volume_id;
    volume.filesystem = metadata.filesystem;
    volume.label = metadata.label;
    volume.total_size = metadata.logical_size_bytes;
    volume.cluster_size = metadata.cluster_size_bytes;
    volume.vss_required = true;
    volume.vss_used = true;
    volume.consistency_level = format::ConsistencyLevel::kApplication;
    for (const auto& mount_point : metadata.mount_points) {
        volume.mount_points.push_back(utf8_path(mount_point));
    }
    manifest.volumes.push_back(std::move(volume));
    return manifest;
}

std::optional<base::Error> release_snapshot(PreparedVolumeSource& prepared) {
    prepared.source.reset();
    auto closed = prepared.snapshot->close();
    if (!closed) {
        return closed.error();
    }
    return std::nullopt;
}

} // namespace

base::Result<WindowsPersonalVolumeBackupResult> backup_windows_personal_volume_with_runtime(
    const WindowsPersonalVolumeBackupRequest& request, const base::CancellationToken& cancellation,
    ports::IProgressSink* progress, IWindowsPersonalBackupRuntime& runtime) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<WindowsPersonalVolumeBackupResult>::failure(validation.error());
    }
    auto prepared = runtime.prepare_source(request.volume_guid_path, cancellation);
    if (!prepared) {
        return base::Result<WindowsPersonalVolumeBackupResult>::failure(prepared.error());
    }

    auto manifest = make_manifest(request, prepared.value().metadata);
    auto session = runtime.create_archive(request, manifest);
    if (!session) {
        (void)release_snapshot(prepared.value());
        return base::Result<WindowsPersonalVolumeBackupResult>::failure(session.error());
    }

    pipeline::BackupPipeline pipeline(*prepared.value().source, *session.value(), progress);
    const pipeline::BackupPlan plan{request.job_id, request.chunk_size_bytes,
                                    request.memory_budget_bytes};
    auto backup = pipeline.run(plan, cancellation);
    auto cleanup_error = release_snapshot(prepared.value());
    if (!backup) {
        return base::Result<WindowsPersonalVolumeBackupResult>::failure(backup.error());
    }
    return base::Result<WindowsPersonalVolumeBackupResult>::success(
        WindowsPersonalVolumeBackupResult{backup.value(), std::move(cleanup_error)});
}

} // namespace detail

base::Result<WindowsPersonalVolumeBackupResult>
backup_windows_personal_volume(const WindowsPersonalVolumeBackupRequest& request,
                               const base::CancellationToken& cancellation,
                               ports::IProgressSink* progress) {
    try {
        auto runtime = detail::make_windows_personal_backup_runtime();
        return detail::backup_windows_personal_volume_with_runtime(request, cancellation, progress,
                                                                   *runtime);
    } catch (...) {
        return base::Result<WindowsPersonalVolumeBackupResult>::failure(base::Error{
            base::ErrorCode::kInternal,
            "Windows personal volume backup failed unexpectedly",
        });
    }
}

} // namespace aegra::apps::worker
