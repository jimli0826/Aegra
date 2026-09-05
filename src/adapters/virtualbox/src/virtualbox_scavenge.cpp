#include "aegra/adapters/virtualbox/virtualbox_scavenge.h"

#include "virtualbox_command_runner.h"
#include "virtualbox_paths.h"

#include <chrono>
#include <string_view>
#include <thread>
#include <vector>

namespace aegra::adapters::virtualbox {
namespace {

constexpr std::string_view kBootCheckVmPrefix = "Aegra-BootCheck-";
constexpr const char* kScavengeFailed = "bootcheck.cleanup_incomplete";

/// `VBoxManage list vms` lines look like: "name" {uuid}. Only prefixed names
/// inside the isolated registry are candidates.
[[nodiscard]] std::vector<std::string> parse_boot_check_vm_names(const std::string_view output) {
    std::vector<std::string> names;
    std::size_t position = 0;
    while (position < output.size()) {
        const auto line_end = output.find('\n', position);
        const auto line =
            output.substr(position, line_end == std::string_view::npos ? output.size() - position
                                                                       : line_end - position);
        position = line_end == std::string_view::npos ? output.size() : line_end + 1;
        const auto open_quote = line.find('"');
        const auto close_quote = line.rfind('"');
        if (open_quote == std::string_view::npos || close_quote <= open_quote) {
            continue;
        }
        const auto name = line.substr(open_quote + 1, close_quote - open_quote - 1);
        if (name.starts_with(kBootCheckVmPrefix)) {
            names.emplace_back(name);
        }
    }
    return names;
}

void power_off_and_wait(detail::VirtualBoxCommandRunner& runner, const std::string& user_home,
                        const std::string& vm_name, const base::CancellationToken cancellation) {
    (void)runner.run({"controlvm", vm_name, "poweroff"}, user_home, cancellation);
    constexpr unsigned kMaximumPolls = 50;
    for (unsigned poll = 0; poll < kMaximumPolls && !cancellation.stop_requested(); ++poll) {
        auto state =
            runner.run({"showvminfo", vm_name, "--machinereadable"}, user_home, cancellation);
        if (!state || state.value().exit_code != 0 ||
            state.value().output.find("VMState=\"running\"") == std::string::npos) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
}

} // namespace

base::Result<std::uint32_t> scavenge_boot_check_home(ports::IProcessLauncher& launcher,
                                                     const std::string& vbox_manage_path,
                                                     const std::string& user_home,
                                                     const base::CancellationToken cancellation) {
    if (!detail::is_trusted_vbox_manage(vbox_manage_path)) {
        return base::Result<std::uint32_t>::failure({base::ErrorCode::kNotFound, kScavengeFailed});
    }
    detail::VirtualBoxCommandRunner runner(launcher, vbox_manage_path);
    auto listed = runner.run({"list", "vms"}, user_home, cancellation);
    if (!listed) {
        return base::Result<std::uint32_t>::failure(listed.error());
    }
    if (listed.value().exit_code != 0) {
        // No usable registry in this home: nothing registered to remove.
        return base::Result<std::uint32_t>::success(0);
    }
    std::uint32_t removed = 0;
    bool failed = false;
    for (const auto& vm_name : parse_boot_check_vm_names(listed.value().output)) {
        power_off_and_wait(runner, user_home, vm_name, cancellation);
        auto unregistered = runner.run({"unregistervm", vm_name}, user_home, cancellation);
        if (unregistered && unregistered.value().exit_code == 0) {
            ++removed;
        } else {
            failed = true;
        }
    }
    if (failed) {
        return base::Result<std::uint32_t>::failure({base::ErrorCode::kIoFailure, kScavengeFailed});
    }
    return base::Result<std::uint32_t>::success(removed);
}

base::Result<std::uint32_t>
scavenge_boot_check_default_registry(ports::IProcessLauncher& launcher,
                                     const std::string& vbox_manage_path,
                                     const base::CancellationToken cancellation) {
    if (!detail::is_trusted_vbox_manage(vbox_manage_path)) {
        return base::Result<std::uint32_t>::failure({base::ErrorCode::kNotFound, kScavengeFailed});
    }
    // Empty user_home => the invoking user's default registry.
    const std::string user_home;
    detail::VirtualBoxCommandRunner runner(launcher, vbox_manage_path);
    auto listed = runner.run({"list", "vms"}, user_home, cancellation);
    if (!listed) {
        return base::Result<std::uint32_t>::failure(listed.error());
    }
    if (listed.value().exit_code != 0) {
        return base::Result<std::uint32_t>::success(0);
    }
    std::uint32_t removed = 0;
    bool failed = false;
    for (const auto& vm_name : parse_boot_check_vm_names(listed.value().output)) {
        // Second gate: only delete VMs carrying the Aegra description marker, so
        // a user's own same-prefixed machine is never touched.
        auto info = runner.run({"showvminfo", vm_name, "--machinereadable"}, user_home, cancellation);
        if (!info || info.value().exit_code != 0 ||
            info.value().output.find("aegra-bootcheck:") == std::string::npos) {
            continue;
        }
        power_off_and_wait(runner, user_home, vm_name, cancellation);
        auto unregistered = runner.run({"unregistervm", vm_name, "--delete"}, user_home, cancellation);
        if (unregistered && unregistered.value().exit_code == 0) {
            ++removed;
        } else {
            failed = true;
        }
    }
    if (failed) {
        return base::Result<std::uint32_t>::failure({base::ErrorCode::kIoFailure, kScavengeFailed});
    }
    return base::Result<std::uint32_t>::success(removed);
}

} // namespace aegra::adapters::virtualbox
