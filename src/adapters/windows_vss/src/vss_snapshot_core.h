#pragma once

#include "aegra/adapters/windows_vss/windows_vss.h"

#include <memory>
#include <span>
#include <vector>

namespace aegra::adapters::windows_vss::detail {

class IVssSnapshotBackend {
  public:
    IVssSnapshotBackend() = default;
    virtual ~IVssSnapshotBackend() = default;
    IVssSnapshotBackend(const IVssSnapshotBackend&) = delete;
    IVssSnapshotBackend& operator=(const IVssSnapshotBackend&) = delete;
    IVssSnapshotBackend(IVssSnapshotBackend&&) = delete;
    IVssSnapshotBackend& operator=(IVssSnapshotBackend&&) = delete;

    [[nodiscard]] virtual base::Result<std::vector<WindowsVssSnapshot>>
    create(std::span<const WindowsVssSnapshotRequest> requests,
           const base::CancellationToken& cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> close() = 0;
    virtual void abandon() noexcept = 0;
};

class VssSnapshotSessionCore final {
  public:
    ~VssSnapshotSessionCore();
    VssSnapshotSessionCore(const VssSnapshotSessionCore&) = delete;
    VssSnapshotSessionCore& operator=(const VssSnapshotSessionCore&) = delete;
    VssSnapshotSessionCore(VssSnapshotSessionCore&&) = delete;
    VssSnapshotSessionCore& operator=(VssSnapshotSessionCore&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<VssSnapshotSessionCore>>
    create(std::span<const WindowsVssSnapshotRequest> requests,
           const base::CancellationToken& cancellation,
           std::unique_ptr<IVssSnapshotBackend> backend);

    [[nodiscard]] std::span<const WindowsVssSnapshot> snapshots() const noexcept;
    [[nodiscard]] bool active() const noexcept;
    [[nodiscard]] base::Result<void> close();

  private:
    VssSnapshotSessionCore(std::unique_ptr<IVssSnapshotBackend> backend,
                           std::vector<WindowsVssSnapshot> snapshots);

    std::unique_ptr<IVssSnapshotBackend> backend_;
    std::vector<WindowsVssSnapshot> snapshots_;
    bool active_{true};
};

[[nodiscard]] bool is_canonical_volume_guid_path(const std::filesystem::path& path) noexcept;
[[nodiscard]] std::unique_ptr<IVssSnapshotBackend> make_windows_vss_backend();

} // namespace aegra::adapters::windows_vss::detail
