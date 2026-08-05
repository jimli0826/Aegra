#pragma once

#include "aegra/apps/worker/windows_personal_backup.h"

#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/format/manifest.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/block_io.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace aegra::apps::worker::detail {

struct PreparedVolumeMetadata final {
    std::filesystem::path volume_guid_path;
    std::vector<std::filesystem::path> mount_points;
    std::string filesystem;
    std::string label;
    std::uint64_t logical_size_bytes{0};
    std::uint32_t cluster_size_bytes{0};
    bool vss_used{false};
    /// Live volume→disk extents (IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS) for Manifest mapping.
    std::vector<aegra::adapters::windows_disk::WindowsVolumeExtent> disk_extents;
};

class ISnapshotLease {
  public:
    ISnapshotLease() = default;
    virtual ~ISnapshotLease() = default;
    ISnapshotLease(const ISnapshotLease&) = delete;
    ISnapshotLease& operator=(const ISnapshotLease&) = delete;
    ISnapshotLease(ISnapshotLease&&) = delete;
    ISnapshotLease& operator=(ISnapshotLease&&) = delete;

    [[nodiscard]] virtual base::Result<void> close() = 0;
};

struct PreparedVolumeSources final {
    std::vector<PreparedVolumeMetadata> metadata;
    std::unique_ptr<ISnapshotLease> snapshot;
    std::vector<std::unique_ptr<ports::IBlockSource>> sources;
};

class IWindowsPersonalBackupRuntime {
  public:
    IWindowsPersonalBackupRuntime() = default;
    virtual ~IWindowsPersonalBackupRuntime() = default;
    IWindowsPersonalBackupRuntime(const IWindowsPersonalBackupRuntime&) = delete;
    IWindowsPersonalBackupRuntime& operator=(const IWindowsPersonalBackupRuntime&) = delete;
    IWindowsPersonalBackupRuntime(IWindowsPersonalBackupRuntime&&) = delete;
    IWindowsPersonalBackupRuntime& operator=(IWindowsPersonalBackupRuntime&&) = delete;

    [[nodiscard]] virtual base::Result<PreparedVolumeSources>
    prepare_sources(const std::vector<std::filesystem::path>& volume_guid_paths,
                    bool exclude_page_and_hibernation_files,
                    const base::CancellationToken& cancellation) = 0;
    [[nodiscard]] virtual base::Result<std::unique_ptr<ports::IBackupSession>>
    create_archive(const WindowsPersonalBackupRequest& request,
                   const format::Manifest& manifest) = 0;
};

[[nodiscard]] base::Result<WindowsPersonalBackupResult>
backup_windows_personal_volumes_with_runtime(const WindowsPersonalBackupRequest& request,
                                             const base::CancellationToken& cancellation,
                                             ports::IProgressSink* progress,
                                             IWindowsPersonalBackupRuntime& runtime);

[[nodiscard]] std::unique_ptr<IWindowsPersonalBackupRuntime> make_windows_personal_backup_runtime();

} // namespace aegra::apps::worker::detail
