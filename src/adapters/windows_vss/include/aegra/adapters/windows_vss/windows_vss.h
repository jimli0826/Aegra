#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace aegra::adapters::windows_vss {

struct WindowsVssSnapshotRequest final {
    std::filesystem::path volume_guid_path;
    std::uint64_t logical_size_bytes{0};
};

struct WindowsVssSnapshot final {
    std::filesystem::path volume_guid_path;
    std::filesystem::path snapshot_device_path;
    std::uint64_t logical_size_bytes{0};
};

class WindowsVssSnapshotSession final {
  public:
    ~WindowsVssSnapshotSession();
    WindowsVssSnapshotSession(const WindowsVssSnapshotSession&) = delete;
    WindowsVssSnapshotSession& operator=(const WindowsVssSnapshotSession&) = delete;
    WindowsVssSnapshotSession(WindowsVssSnapshotSession&&) = delete;
    WindowsVssSnapshotSession& operator=(WindowsVssSnapshotSession&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsVssSnapshotSession>>
    create(std::span<const WindowsVssSnapshotRequest> requests,
           const base::CancellationToken& cancellation);
    [[nodiscard]] static bool
    is_canonical_volume_guid_path(const std::filesystem::path& path) noexcept;

    [[nodiscard]] std::span<const WindowsVssSnapshot> snapshots() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] base::Result<void> close();

  private:
    struct Impl;

    explicit WindowsVssSnapshotSession(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

/// Probe whether the volume can be added to a VSS backup snapshot set (IsVolumeSupported).
/// Does not create a snapshot. Failures return Result error; false means provider rejects the volume.
[[nodiscard]] base::Result<bool>
is_volume_snapshot_supported(const std::filesystem::path& volume_guid_path);

} // namespace aegra::adapters::windows_vss
