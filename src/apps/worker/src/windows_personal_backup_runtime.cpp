#include "windows_personal_backup_runtime.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_vss/windows_vss.h"

#include <algorithm>
#include <cwctype>
#include <memory>
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
    if (!windows_vss::WindowsVssSnapshotSession::is_canonical_volume_guid_path(volume_guid_path)) {
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

PreparedVolumeMetadata make_metadata(const windows_disk::WindowsVolumeInfo& volume) {
    return PreparedVolumeMetadata{
        volume.volume_guid_path, volume.mount_points,       volume.filesystem, volume.label,
        volume.total_size_bytes, volume.cluster_size_bytes,
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

base::Result<PreparedVolumeSource> snapshot_volume(const windows_disk::WindowsVolumeInfo& volume,
                                                   const base::CancellationToken& cancellation) {
    const windows_vss::WindowsVssSnapshotRequest snapshot_request{
        volume.volume_guid_path,
        volume.total_size_bytes,
    };
    auto session = windows_vss::WindowsVssSnapshotSession::create(
        std::span<const windows_vss::WindowsVssSnapshotRequest>(&snapshot_request, 1),
        cancellation);
    if (!session) {
        return base::Result<PreparedVolumeSource>::failure(session.error());
    }
    const auto snapshots = session.value()->snapshots();
    if (snapshots.size() != 1) {
        return base::Result<PreparedVolumeSource>::failure(
            base::Error{base::ErrorCode::kInternal, "VSS returned an invalid source count"});
    }

    auto source =
        windows_disk::WindowsBlockSource::open(windows_disk::WindowsBlockSourceOpenRequest{
            snapshots.front().snapshot_device_path,
            windows_disk::WindowsBlockSourceKind::kVssSnapshot,
            snapshots.front().logical_size_bytes,
        });
    if (!source) {
        return base::Result<PreparedVolumeSource>::failure(source.error());
    }
    std::unique_ptr<ISnapshotLease> lease =
        std::make_unique<WindowsSnapshotLease>(std::move(session).value());
    std::unique_ptr<ports::IBlockSource> block_source = std::move(source).value();
    return base::Result<PreparedVolumeSource>::success(
        PreparedVolumeSource{make_metadata(volume), std::move(lease), std::move(block_source)});
}

class WindowsPersonalBackupRuntime final : public IWindowsPersonalBackupRuntime {
  public:
    [[nodiscard]] base::Result<PreparedVolumeSource>
    prepare_source(const std::filesystem::path& volume_guid_path,
                   const base::CancellationToken& cancellation) override {
        auto volume = resolve_volume(volume_guid_path);
        if (!volume) {
            return base::Result<PreparedVolumeSource>::failure(volume.error());
        }
        return snapshot_volume(volume.value(), cancellation);
    }

    [[nodiscard]] base::Result<std::unique_ptr<ports::IBackupSession>>
    create_archive(const WindowsPersonalVolumeBackupRequest& request,
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
        archive_request.source_index = 0;
        archive_request.split_size_bytes = request.split_size_bytes;
        archive_request.kdf_parameters = {request.kdf_opslimit, request.kdf_memlimit_bytes};
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
