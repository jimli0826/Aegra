#pragma once

#include <string>

namespace aegra::adapters::hyperv {

/// Resolves the trusted absolute Windows PowerShell path
/// (<System32>\WindowsPowerShell\v1.0\powershell.exe). Returns an empty string
/// when the executable is missing. Hyper-V availability itself is probed by the
/// provider's inspect().
[[nodiscard]] std::string discover_powershell_path();

/// True when the Hyper-V Virtual Machine Management service is installed.
/// The service may be stopped; run-time usability is checked by provider::inspect().
[[nodiscard]] bool is_hyperv_installed() noexcept;

} // namespace aegra::adapters::hyperv
