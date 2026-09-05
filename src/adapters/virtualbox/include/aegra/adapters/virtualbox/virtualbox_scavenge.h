#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::ports {
class IProcessLauncher;
}

namespace aegra::adapters::virtualbox {

/// Powers off and unregisters every `Aegra-BootCheck-*` VM registered in ONE
/// isolated per-job VBOX_USER_HOME. Never touches the user's global VirtualBox
/// registry: the caller passes the job-private home, and only VMs matching the
/// BootCheck name prefix inside that registry are removed. Returns the number
/// of VMs unregistered; a missing/empty registry is success with zero.
[[nodiscard]] base::Result<std::uint32_t>
scavenge_boot_check_home(ports::IProcessLauncher& launcher, const std::string& vbox_manage_path,
                         const std::string& user_home, base::CancellationToken cancellation);

/// Powers off and unregisters (with --delete) orphaned `Aegra-BootCheck-*` VMs
/// from the INVOKING USER's default VirtualBox registry (no VBOX_USER_HOME
/// override). Double-gated on the name prefix AND the `aegra-bootcheck:` marker
/// written into each job VM's description, so a user's own machine sharing the
/// name prefix is never removed. Used only when the host runs under the user's
/// token (user-visible job path). Returns the number of VMs unregistered.
[[nodiscard]] base::Result<std::uint32_t>
scavenge_boot_check_default_registry(ports::IProcessLauncher& launcher,
                                     const std::string& vbox_manage_path,
                                     base::CancellationToken cancellation);

} // namespace aegra::adapters::virtualbox
