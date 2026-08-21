#include "windows_personal_backup_runtime.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_vss/windows_vss.h"

#include <algorithm>
#include <cwctype>
#include <memory>
#include <sstream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::worker::detail {
namespace {

namespace personal_archive = adapters::personal_archive;
namespace windows_disk = adapters::windows_disk;
namespace windows_vss = adapters::windows_vss;

std::wstring normalized_path(const std::filesystem::path& path) {
    auto value = path.native();
    std::ranges::transform(value, value.begin(),
                           [](const wchar_t character) { return std::towlower(character); });
    return value;
}

base::Result<windows_disk::WindowsVolumeInfo>
resolve_volume(const std::filesystem::path& volume_guid_path) {
    if (!windows_disk::WindowsBlockSource::is_canonical_volume_guid_path(volume_guid_path)) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "backup source is not a canonical Windows Volume GUID path",
        });
    }
    auto inventory = windows_disk::WindowsVolumeEnumerator::enumerate();
    if (!inventory) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(inventory.error());
    }
    const auto expected = normalized_path(volume_guid_path);
    const auto selected = std::ranges::find_if(inventory.value(), [&expected](const auto& volume) {
        return normalized_path(volume.volume_guid_path) == expected;
    });
    if (selected == inventory.value().end()) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(
            base::Error{base::ErrorCode::kNotFound, "requested Windows volume was not found"});
    }
    if (!selected->volume_size_available || selected->total_size_bytes == 0) {
        return base::Result<windows_disk::WindowsVolumeInfo>::failure(base::Error{
            base::ErrorCode::kIoFailure,
            "requested Windows volume has no reliable logical size",
        });
    }
    return base::Result<windows_disk::WindowsVolumeInfo>::success(*selected);
}

PreparedVolumeMetadata make_metadata(const windows_disk::WindowsVolumeInfo& volume,
                                     const bool vss_used) {
    PreparedVolumeMetadata metadata;
    metadata.volume_guid_path = volume.volume_guid_path;
    metadata.mount_points = volume.mount_points;
    metadata.filesystem = volume.filesystem;
    metadata.label = volume.label;
    metadata.logical_size_bytes = volume.total_size_bytes;
    metadata.free_size_known = volume.filesystem_metadata_available;
    if (metadata.free_size_known) {
        metadata.free_size_bytes = volume.free_size_bytes > volume.total_size_bytes
                                       ? volume.total_size_bytes
                                       : volume.free_size_bytes;
    } else {
        metadata.free_size_bytes = 0;
    }
    metadata.cluster_size_bytes = volume.cluster_size_bytes;
    metadata.vss_used = vss_used;
    metadata.disk_extents = volume.extents;
    return metadata;
}

class WindowsSnapshotLease final : public ISnapshotLease {
  public:
    explicit WindowsSnapshotLease(std::unique_ptr<windows_vss::WindowsVssSnapshotSession> session)
        : session_(std::move(session)) {}

    [[nodiscard]] base::Result<void> close() override { return session_->close(); }

  private:
    std::unique_ptr<windows_vss::WindowsVssSnapshotSession> session_;
};

std::string utf8_or_empty(const std::filesystem::path& path) {
    const auto encoded = path.generic_u8string();
    std::string text;
    text.reserve(encoded.size());
    for (const auto value : encoded) {
        text.push_back(static_cast<char>(value));
    }
    return text;
}

// Filesystem name is only a candidate filter; IsVolumeSupported is authoritative.
base::Result<std::vector<std::size_t>>
vss_volume_indices(const std::vector<windows_disk::WindowsVolumeInfo>& volumes) {
    std::vector<std::size_t> result;
    for (std::size_t index = 0; index < volumes.size(); ++index) {
        if (!windows_disk::supports_vss_snapshot(volumes[index])) {
            continue;
        }
        auto supported =
            windows_vss::is_volume_snapshot_supported(volumes[index].volume_guid_path);
        if (!supported) {
            return base::Result<std::vector<std::size_t>>::failure(supported.error());
        }
        if (supported.value()) {
            result.push_back(index);
        } else if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            log->field("vss_fallback", "not_supported");
            log->field("volume", utf8_or_empty(volumes[index].volume_guid_path));
        }
    }
    return base::Result<std::vector<std::size_t>>::success(std::move(result));
}

base::Result<std::unique_ptr<windows_vss::WindowsVssSnapshotSession>>
create_vss_session(const std::vector<windows_disk::WindowsVolumeInfo>& volumes,
                   const std::span<const std::size_t> indices,
                   const base::CancellationToken& cancellation) {
    if (indices.empty()) {
        return base::Result<std::unique_ptr<windows_vss::WindowsVssSnapshotSession>>::success({});
    }
    std::vector<windows_vss::WindowsVssSnapshotRequest> requests;
    requests.reserve(indices.size());
    for (const auto index : indices) {
        const auto& volume = volumes[index];
        requests.push_back({volume.volume_guid_path, volume.total_size_bytes});
    }
    return windows_vss::WindowsVssSnapshotSession::create(requests, cancellation);
}

void log_free_skip(const windows_disk::FreeSkipPlan& plan, const bool applied) {
    if (auto* log = WorkerTaskLog::active(); log == nullptr) {
        return;
    }
    auto* log = WorkerTaskLog::active();
    if (!applied) {
        log->field("bitmap_optimization", "disabled");
        log->field("filesystem", plan.filesystem.empty() ? "?" : plan.filesystem);
        return;
    }
    log->field("bitmap_optimization", "enabled");
    log->field("filesystem", plan.filesystem);
    log->field_bytes("free_bytes", plan.free_bytes);
    log->field_bytes("total_bytes", plan.total_bytes);
    log->field_u64("free_ranges", plan.free_ranges.size());
    log->field_bytes("protected_prefix", plan.protected_prefix_bytes);
}

base::Result<std::unique_ptr<ports::IBlockSource>> open_volume_source(
    const windows_disk::WindowsVolumeInfo& volume, const windows_vss::WindowsVssSnapshot* snapshot,
    const bool exclude_page_and_hibernation_files, const std::uint32_t archive_block_size_bytes) {
    const bool use_vss = snapshot != nullptr;
    const auto path = use_vss ? snapshot->snapshot_device_path : volume.volume_guid_path;
    const auto kind = use_vss ? windows_disk::WindowsBlockSourceKind::kVssSnapshot
                              : windows_disk::WindowsBlockSourceKind::kRawVolume;
    const auto size = use_vss ? snapshot->logical_size_bytes : volume.total_size_bytes;
    auto source = windows_disk::WindowsBlockSource::open({path, kind, size});
    if (!source) {
        return base::Result<std::unique_ptr<ports::IBlockSource>>::failure(source.error());
    }
    std::unique_ptr<ports::IBlockSource> result = std::move(source).value();

    // Free-cluster skip + pagefile/hiber/swap exclusion on the **same** device path used for
    // reads (live GUID or VSS snapshot root).
    auto free_plan = windows_disk::build_free_skip_plan(path, volume.filesystem, size,
                                                        volume.cluster_size_bytes);
    free_plan.total_bytes = size;
    if (exclude_page_and_hibernation_files) {
        const auto excluded = windows_disk::merge_page_and_hibernation_exclusions(
            free_plan, path, volume.cluster_size_bytes);
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            if (excluded > 0) {
                log->field_bytes("excluded_page_hiber_swap_candidate_bytes", excluded);
                log->field("exclusion_root", utf8_or_empty(path));
                log->field("exclusion_mode", use_vss ? "vss_snapshot" : "live_volume");
            } else if (use_vss) {
                log->field("page_hiber_swap", "none_on_snapshot_root");
                log->field("exclusion_root", utf8_or_empty(path));
            }
        }
    }
    windows_disk::align_free_skip_plan(free_plan, archive_block_size_bytes);
    if (free_plan.applied && !free_plan.free_ranges.empty()) {
        auto wrapped =
            windows_disk::FreeSkipBlockSource::wrap(std::move(result), std::move(free_plan));
        if (!wrapped) {
            return base::Result<std::unique_ptr<ports::IBlockSource>>::failure(wrapped.error());
        }
        log_free_skip(wrapped.value()->plan(), true);
        result = std::move(wrapped).value();
    } else {
        log_free_skip(free_plan, false);
    }
    return base::Result<std::unique_ptr<ports::IBlockSource>>::success(std::move(result));
}

base::Result<PreparedVolumeSources>
prepare_volume_sources(const std::vector<windows_disk::WindowsVolumeInfo>& volumes,
                       const bool exclude_page_and_hibernation_files,
                       const std::uint32_t archive_block_size_bytes,
                       const base::CancellationToken& cancellation) {
    auto vss_indices = vss_volume_indices(volumes);
    if (!vss_indices) {
        return base::Result<PreparedVolumeSources>::failure(vss_indices.error());
    }
    auto session = create_vss_session(volumes, vss_indices.value(), cancellation);
    if (!session) {
        return base::Result<PreparedVolumeSources>::failure(session.error());
    }
    const auto snapshots = session.value() ? session.value()->snapshots()
                                           : std::span<const windows_vss::WindowsVssSnapshot>{};
    if (snapshots.size() != vss_indices.value().size()) {
        return base::Result<PreparedVolumeSources>::failure(
            base::Error{base::ErrorCode::kInternal, "VSS returned an invalid source count"});
    }
    // Map volume index -> snapshot pointer for volumes that actually entered the set.
    std::vector<const windows_vss::WindowsVssSnapshot*> snapshot_by_volume(volumes.size(), nullptr);
    for (std::size_t i = 0; i < vss_indices.value().size(); ++i) {
        snapshot_by_volume[vss_indices.value()[i]] = &snapshots[i];
    }
    PreparedVolumeSources result;
    result.metadata.reserve(volumes.size());
    result.sources.resize(volumes.size());
    for (std::size_t index = 0; index < volumes.size(); ++index) {
        const auto* snapshot = snapshot_by_volume[index];
        auto source = open_volume_source(
            volumes[index], snapshot, exclude_page_and_hibernation_files, archive_block_size_bytes);
        if (!source) {
            return base::Result<PreparedVolumeSources>::failure(source.error());
        }
        result.metadata.push_back(make_metadata(volumes[index], snapshot != nullptr));
        result.sources[index] = std::move(source).value();
    }
    if (session.value()) {
        result.snapshot = std::make_unique<WindowsSnapshotLease>(std::move(session).value());
    }
    return base::Result<PreparedVolumeSources>::success(std::move(result));
}

class WindowsPersonalBackupRuntime final : public IWindowsPersonalBackupRuntime {
  public:
    [[nodiscard]] base::Result<PreparedVolumeSources>
    prepare_sources(const std::vector<std::filesystem::path>& volume_guid_paths,
                    const bool exclude_page_and_hibernation_files,
                    const std::uint32_t archive_block_size_bytes,
                    const base::CancellationToken& cancellation) override {
        std::vector<windows_disk::WindowsVolumeInfo> volumes;
        volumes.reserve(volume_guid_paths.size());
        for (const auto& path : volume_guid_paths) {
            auto volume = resolve_volume(path);
            if (!volume) {
                return base::Result<PreparedVolumeSources>::failure(volume.error());
            }
            volumes.push_back(std::move(volume).value());
        }
        return prepare_volume_sources(volumes, exclude_page_and_hibernation_files,
                                      archive_block_size_bytes, cancellation);
    }

    [[nodiscard]] base::Result<std::unique_ptr<ports::IBackupSession>>
    create_archive(const WindowsPersonalBackupRequest& request,
                   const format::Manifest& manifest) override {
        personal_archive::ArchiveCreateRequest archive_request{
            request.destination,
            manifest,
            request.password,
        };
        archive_request.encryption_enabled = request.encryption_enabled;
        archive_request.file_uuid = request.file_uuid;
        archive_request.backup_set_uuid = request.backup_set_uuid;
        archive_request.block_size = request.block_size_bytes;
        archive_request.chunk_size = request.chunk_size_bytes;
        archive_request.split_size_bytes = request.split_size_bytes;
        archive_request.kdf_parameters = {request.kdf_opslimit, request.kdf_memlimit_bytes};
        archive_request.parent_source = request.parent_source;
        archive_request.parent_password = request.parent_password;
        archive_request.deduplication_enabled = request.deduplication_enabled;
        auto session = personal_archive::PersonalArchiveSession::create(archive_request);
        if (!session) {
            return base::Result<std::unique_ptr<ports::IBackupSession>>::failure(session.error());
        }
        std::unique_ptr<ports::IBackupSession> result = std::move(session).value();
        return base::Result<std::unique_ptr<ports::IBackupSession>>::success(std::move(result));
    }
};

} // namespace

std::unique_ptr<IWindowsPersonalBackupRuntime> make_windows_personal_backup_runtime() {
    return std::make_unique<WindowsPersonalBackupRuntime>();
}

} // namespace aegra::apps::worker::detail
