#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/process_launcher.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::adapters::hyperv::detail {

struct PowerShellResult final {
    std::uint32_t exit_code{0};
    std::string output;
};

/// Runs one PowerShell script via -EncodedCommand (base64 UTF-16LE), which
/// removes every quoting concern for embedded paths. Non-interactive, no
/// profile, captured combined output.
class PowerShellRunner final {
  public:
    PowerShellRunner(ports::IProcessLauncher& launcher, std::string powershell_path);

    [[nodiscard]] base::Result<PowerShellResult> run(std::string_view script,
                                                     base::CancellationToken cancellation);

  private:
    ports::IProcessLauncher* launcher_{nullptr};
    std::string powershell_path_;
};

/// Single-quotes a value for embedding in a PowerShell script ('' escaping).
[[nodiscard]] std::string quote_powershell(std::string_view value);

} // namespace aegra::adapters::hyperv::detail
