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

} // namespace aegra::adapters::virtualbox
