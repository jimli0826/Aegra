#include "aegra/adapters/windows_vss/windows_vss.h"

#include "vss_snapshot_core.h"

#include <utility>

namespace aegra::adapters::windows_vss {

struct WindowsVssSnapshotSession::Impl final {
    std::unique_ptr<detail::VssSnapshotSessionCore> core;
};

WindowsVssSnapshotSession::WindowsVssSnapshotSession(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

WindowsVssSnapshotSession::~WindowsVssSnapshotSession() = default;

base::Result<std::unique_ptr<WindowsVssSnapshotSession>>
WindowsVssSnapshotSession::create(const std::span<const WindowsVssSnapshotRequest> requests,
                                  const base::CancellationToken& cancellation) {
    auto core = detail::VssSnapshotSessionCore::create(requests, cancellation,
                                                       detail::make_windows_vss_backend());
    if (!core) {
        return base::Result<std::unique_ptr<WindowsVssSnapshotSession>>::failure(core.error());
    }
    auto impl = std::make_unique<Impl>();
    impl->core = std::move(core).value();
    return base::Result<std::unique_ptr<WindowsVssSnapshotSession>>::success(
        std::unique_ptr<WindowsVssSnapshotSession>(new WindowsVssSnapshotSession(std::move(impl))));
}

bool WindowsVssSnapshotSession::is_canonical_volume_guid_path(
    const std::filesystem::path& path) noexcept {
    return detail::is_canonical_volume_guid_path(path);
}

std::span<const WindowsVssSnapshot> WindowsVssSnapshotSession::snapshots() const noexcept {
    return impl_->core->snapshots();
}

bool WindowsVssSnapshotSession::active() const noexcept { return impl_->core->active(); }

base::Result<void> WindowsVssSnapshotSession::close() { return impl_->core->close(); }

base::Result<bool> is_volume_snapshot_supported(const std::filesystem::path& volume_guid_path) {
    return detail::probe_volume_snapshot_supported(volume_guid_path);
}

} // namespace aegra::adapters::windows_vss
