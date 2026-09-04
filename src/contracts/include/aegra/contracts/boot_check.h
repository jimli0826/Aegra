#pragma once

#include <cstdint>

namespace aegra::contracts {

enum class BootCheckHypervisor : std::uint8_t {
    kVirtualBox = 1,
    kHyperV = 2,
};

[[nodiscard]] constexpr bool
is_known_boot_check_hypervisor(const BootCheckHypervisor hypervisor) noexcept {
    return hypervisor == BootCheckHypervisor::kVirtualBox ||
           hypervisor == BootCheckHypervisor::kHyperV;
}

/// Historical manifest field: the guest probe protocol the source machine
/// supported when the backup was taken. Boot confirmation no longer uses a
/// guest probe; the field stays for manifest V7 stability.
inline constexpr std::uint16_t kBootCheckProbeProtocolVersion = 1;

} // namespace aegra::contracts
