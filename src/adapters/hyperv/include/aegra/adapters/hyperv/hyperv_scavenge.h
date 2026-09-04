#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::ports {
class IProcessLauncher;
}

namespace aegra::adapters::hyperv {

/// Turns off and removes every orphaned BootCheck VM in the host's Hyper-V
/// inventory. Hyper-V has no per-job registry, so candidates are double-gated:
/// the name must start with `Aegra-BootCheck-` AND the VM Notes must carry the
/// `aegra-bootcheck:` marker written at creation. Returns the number of VMs
/// removed; an absent/idle Hyper-V stack is success with zero.
[[nodiscard]] base::Result<std::uint32_t>
scavenge_boot_check_vms(ports::IProcessLauncher& launcher, const std::string& powershell_path,
                        base::CancellationToken cancellation);

} // namespace aegra::adapters::hyperv
