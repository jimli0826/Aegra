#include "vss_snapshot_core.h"

#include <algorithm>
#include <cwctype>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace aegra::adapters::windows_vss::detail {
namespace {

constexpr std::wstring_view kVolumePrefix = LR"(\\?\Volume{)";
constexpr std::size_t kGuidTextLength = 36;

bool is_hex_digit(const wchar_t value) noexcept {
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

bool is_guid_text(const std::wstring_view value) noexcept {
    if (value.size() != kGuidTextLength) {
        return false;
    }
    for (std::size_t index = 0; index < value.size(); ++index) {
        const bool separator = index == 8U || index == 13U || index == 18U || index == 23U;
        if ((separator && value[index] != L'-') || (!separator && !is_hex_digit(value[index]))) {
            return false;
        }
    }
    return true;
}

std::wstring normalized_path(const std::filesystem::path& path) {
    auto value = path.native();
    std::ranges::transform(value, value.begin(),
                           [](const wchar_t character) { return std::towlower(character); });
    return value;
}

base::Result<void> validate_requests(const std::span<const WindowsVssSnapshotRequest> requests,
                                     const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCancelled, "VSS snapshot creation cancelled"});
    }
    if (requests.empty()) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "VSS snapshot request list is empty",
        });
    }

    std::unordered_set<std::wstring> identities;
    for (const auto& request : requests) {
        if (!is_canonical_volume_guid_path(request.volume_guid_path) ||
            request.logical_size_bytes == 0) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "VSS snapshot request has an invalid volume identity or size",
            });
        }
        if (!identities.insert(normalized_path(request.volume_guid_path)).second) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kConflict,
                "VSS snapshot request contains a duplicate volume",
            });
        }
    }
    return base::Result<void>::success();
}

bool matches_requests(const std::span<const WindowsVssSnapshotRequest> requests,
                      const std::span<const WindowsVssSnapshot> snapshots) {
    if (requests.size() != snapshots.size()) {
        return false;
    }
    for (std::size_t index = 0; index < requests.size(); ++index) {
        if (normalized_path(requests[index].volume_guid_path) !=
                normalized_path(snapshots[index].volume_guid_path) ||
            requests[index].logical_size_bytes != snapshots[index].logical_size_bytes ||
            snapshots[index].snapshot_device_path.empty()) {
            return false;
        }
    }
    return true;
}

} // namespace

bool is_canonical_volume_guid_path(const std::filesystem::path& path) noexcept {
    const auto value = std::wstring_view(path.native());
    constexpr auto kExpectedSize = kVolumePrefix.size() + kGuidTextLength + 2U;
    if (value.size() != kExpectedSize || value.substr(0, kVolumePrefix.size()) != kVolumePrefix ||
        value[kExpectedSize - 2U] != L'}' || value.back() != L'\\') {
        return false;
    }
    return is_guid_text(value.substr(kVolumePrefix.size(), kGuidTextLength));
}

VssSnapshotSessionCore::VssSnapshotSessionCore(std::unique_ptr<IVssSnapshotBackend> backend,
                                               std::vector<WindowsVssSnapshot> snapshots)
    : backend_(std::move(backend)), snapshots_(std::move(snapshots)) {}

VssSnapshotSessionCore::~VssSnapshotSessionCore() {
    if (active_) {
        backend_->abandon();
    }
}

base::Result<std::unique_ptr<VssSnapshotSessionCore>>
VssSnapshotSessionCore::create(const std::span<const WindowsVssSnapshotRequest> requests,
                               const base::CancellationToken& cancellation,
                               std::unique_ptr<IVssSnapshotBackend> backend) {
    if (!backend) {
        return base::Result<std::unique_ptr<VssSnapshotSessionCore>>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "VSS backend is missing"});
    }
    auto validation = validate_requests(requests, cancellation);
    if (!validation) {
        return base::Result<std::unique_ptr<VssSnapshotSessionCore>>::failure(validation.error());
    }
    auto snapshots = backend->create(requests, cancellation);
    if (!snapshots) {
        return base::Result<std::unique_ptr<VssSnapshotSessionCore>>::failure(snapshots.error());
    }
    if (!matches_requests(requests, snapshots.value())) {
        backend->abandon();
        return base::Result<std::unique_ptr<VssSnapshotSessionCore>>::failure(base::Error{
            base::ErrorCode::kInternal,
            "VSS backend returned an inconsistent snapshot mapping",
        });
    }
    return base::Result<std::unique_ptr<VssSnapshotSessionCore>>::success(
        std::unique_ptr<VssSnapshotSessionCore>(
            new VssSnapshotSessionCore(std::move(backend), std::move(snapshots).value())));
}

std::span<const WindowsVssSnapshot> VssSnapshotSessionCore::snapshots() const noexcept {
    return snapshots_;
}

bool VssSnapshotSessionCore::active() const noexcept { return active_; }

base::Result<void> VssSnapshotSessionCore::close() {
    if (!active_) {
        return base::Result<void>::success();
    }
    auto result = backend_->close();
    if (result) {
        active_ = false;
    }
    return result;
}

} // namespace aegra::adapters::windows_vss::detail
