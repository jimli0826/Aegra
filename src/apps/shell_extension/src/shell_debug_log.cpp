#include "pch.h"

#include "shell_debug_log.h"

namespace aegra::shell {

void shell_debug_log(const std::wstring& /*message*/) noexcept {
    // Debug file/OutputDebugString logging is disabled for production Explorer loads.
    // Re-enable temporarily when diagnosing Shell Extension issues.
}

} // namespace aegra::shell
