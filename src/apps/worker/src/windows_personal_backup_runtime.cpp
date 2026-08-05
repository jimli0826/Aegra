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
    return PreparedVolumeMetadata{
        volume.volume_guid_path, volume.mount_points,       volume.filesystem, volume.label,
        volume.total_size_bytes, volume.cluster_size_bytes, vss_used,
    };
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
            log->info(std::string("VSS not supported for volume; using raw: ") +
                      utf8_or_empty(volumes[index].volume_guid_path));
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
    if (auto* log = WorkerTaskLog::active(); log != nullptr) {
        if (!applied) {
            log->info(std::string("Volume Bitmap optimization disabled for filesystem \"") +
                      (plan.filesystem.empty() ? "?" : plan.filesystem) +
                      "\"; reading all blocks");
            return;
        }
        std::ostringstream line;
        line << "Volume Bitmap optimization (" << plan.filesystem << "): free_bytes="
             << plan.free_bytes << " / total_bytes=" << plan.total_bytes
             << " free_ranges=" << plan.free_ranges.size()
             << " protected_prefix=" << plan.protected_prefix_bytes;
        log->info(line.str());
    }
}

base::Result<std::unique_ptr<ports::IBlockSource>>
open_volume_source(const windows_disk::WindowsVolumeInfo& volume,
                   const windows_vss::WindowsVssSnapshot* snapshot,
                   const bool exclude_page_and_hibernation_files) {
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
    // reads (AipCopy: ExcludeJunkFiles on swStaticVolume — live GUID or VSS snapshot root).
    auto free_plan =
        windows_disk::build_free_skip_plan(path, volume.filesystem, size, volume.cluster_size_bytes);
    free_plan.total_bytes = size;
    if (exclude_page_and_hibernation_files) {
        const auto excluded = windows_disk::merge_page_and_hibernation_exclusions(
            free_plan, path, volume.cluster_size_bytes);
        if (auto* log = WorkerTaskLog::active(); log != nullptr) {
            if (excluded > 0) {
                std::ostringstream line;
                line << "Excluded page/hiber/swap coverage " << excluded << " byte(s) on "
                     << utf8_or_empty(path) << (use_vss ? " (VSS snapshot root)" : " (live volume)");
                log->info(line.str());
            } else if (use_vss) {
                log->info("page/hiber/swap exclusion: no extents on snapshot root " +
                          utf8_or_empty(path) + " (absent or unreadable; continuing)");
            }
        }
    }
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
        auto source =
            open_volume_source(volumes[index], snapshot, exclude_page_and_hibernation_files);
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
        return prepare_volume_sources(volumes, exclude_page_and_hibernation_files, cancellation);
    }

    [[nodiscard]] base::Result<std::unique_ptr<ports::IBackupSession>>
    create_archive(const WindowsPersonalBackupRequest& request,
                   const format::Manifest& manifest) override {
        personal_archive::ArchiveCreateRequest archive_request{
            request.destination,
            manifest,
            request.password,
        };
        archive_request.file_uuid = request.file_uuid;
        archive_request.backup_set_uuid = request.backup_set_uuid;
        archive_request.block_size = request.block_size_bytes;
        archive_request.chunk_size = request.chunk_size_bytes;
        archive_request.split_size_bytes = request.split_size_bytes;
        archive_request.kdf_parameters = {request.kdf_opslimit, request.kdf_memlimit_bytes};
        archive_request.parent_source = request.parent_source;
        archive_request.parent_password = request.parent_password;
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
