#pragma once

#include "aegra/base/result.h"
#include "aegra/ports/one_time_boot.h"
#include "aegra/ports/process_launcher.h"

#include <functional>
#include <memory>
#include <string>
#include <string_view>

namespace aegra::adapters::windows_pe {

/// Optional diagnostic sink: receives one line per bcdedit invocation
/// (arguments, exit code, condensed output). Never carries secrets — this flow
/// has none. Wired by the composition root to the service log for field triage.
using OneTimeBootDiagnosticLog = std::function<void(std::string_view)>;

struct OneTimeBootControllerOpenRequest final {
    /// Required. Non-owning: the launcher must outlive the controller. The
    /// composition root wires the windows_process implementation.
    ports::IProcessLauncher* process_launcher{nullptr};
    /// Optional absolute path override for bcdedit.exe (diagnostics only).
    /// Empty resolves `<system directory>\bcdedit.exe`.
    std::string bcdedit_path_utf8;
    /// Optional per-command diagnostic sink (see OneTimeBootDiagnosticLog).
    OneTimeBootDiagnosticLog diagnostic_log;
};

/// bcdedit-based one-time boot controller (ADR-0026 A). The product boot entry
/// and its ramdisk device options use fixed product GUIDs created explicitly
/// with `bcdedit /create {guid}`, so no localized bcdedit text is ever parsed.
[[nodiscard]] base::Result<std::unique_ptr<ports::IOneTimeBootController>>
open_one_time_boot_controller(const OneTimeBootControllerOpenRequest& request);

} // namespace aegra::adapters::windows_pe
