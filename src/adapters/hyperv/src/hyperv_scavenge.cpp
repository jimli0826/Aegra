#include "aegra/adapters/hyperv/hyperv_scavenge.h"

#include "hyperv_powershell.h"

#include <string_view>

namespace aegra::adapters::hyperv {

base::Result<std::uint32_t> scavenge_boot_check_vms(ports::IProcessLauncher& launcher,
                                                    const std::string& powershell_path,
                                                    const base::CancellationToken cancellation) {
    if (powershell_path.empty()) {
        return base::Result<std::uint32_t>::success(0);
    }
    detail::PowerShellRunner runner(launcher, powershell_path);
    // Double gate: BootCheck name prefix AND the creation-time Notes marker.
    // An absent or stopped Hyper-V stack is success with zero removals; the
    // module can be installed while vmms is not running, and Get-VM then throws.
    auto result = runner.run("$c = Get-Command Get-VM -ErrorAction SilentlyContinue; "
                             "$s = Get-Service vmms -ErrorAction SilentlyContinue; "
                             "if (-not $c -or -not $s -or $s.Status -ne 'Running') { "
                             "Write-Output 'aegra-hyperv-removed 0'; exit 0 } "
                             "$removed = 0; "
                             "Get-VM | Where-Object { $_.Name -like 'Aegra-BootCheck-*' "
                             "-and $_.Notes -like 'aegra-bootcheck:*' } | ForEach-Object { "
                             "Stop-VM -VM $_ -TurnOff -Force -ErrorAction SilentlyContinue; "
                             "Remove-VM -VM $_ -Force; $removed++ }; "
                             "Write-Output ('aegra-hyperv-removed ' + $removed)",
                             cancellation);
    if (!result) {
        return base::Result<std::uint32_t>::failure(result.error());
    }
    if (result.value().exit_code != 0) {
        return base::Result<std::uint32_t>::failure(
            {base::ErrorCode::kIoFailure, "bootcheck.cleanup_incomplete"});
    }
    constexpr std::string_view kMarker = "aegra-hyperv-removed ";
    const auto position = result.value().output.find(kMarker);
    if (position == std::string::npos) {
        return base::Result<std::uint32_t>::success(0);
    }
    std::uint32_t removed = 0;
    for (auto cursor = position + kMarker.size(); cursor < result.value().output.size(); ++cursor) {
        const char digit = result.value().output[cursor];
        if (digit < '0' || digit > '9' || removed > 100'000U) {
            break;
        }
        removed = removed * 10U + static_cast<std::uint32_t>(digit - '0');
    }
    return base::Result<std::uint32_t>::success(removed);
}

} // namespace aegra::adapters::hyperv
