#include "aegra/adapters/virtualbox/virtualbox_boot_check_provider.h"

#include "virtualbox_command_runner.h"
#include "virtualbox_paths.h"

#include "aegra/ports/process_launcher.h"

#include <windows.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::adapters::virtualbox {
namespace {

using detail::VirtualBoxCommandRunner;
using detail::VirtualBoxJobLayout;
using virtualization::BootCheckVmState;

constexpr const char* kProviderUnavailable = "bootcheck.provider_unavailable";
constexpr const char* kVmCreateFailed = "bootcheck.vm_create_failed";
constexpr const char* kVmStartFailed = "bootcheck.vm_start_failed";
constexpr const char* kCleanupIncomplete = "bootcheck.cleanup_incomplete";

base::Error stable_error(const base::ErrorCode code, const char* message) {
    return base::Error{code, message};
}

class CleanupDeadline final {
  public:
    CleanupDeadline()
        : watchdog_([this](const std::stop_token stopped) {
              std::unique_lock lock(mutex_);
              changed_.wait_for(lock, stopped, std::chrono::seconds(15), [] { return false; });
              if (!stopped.stop_requested()) {
                  cancellation_.request_stop();
              }
          }) {}

    ~CleanupDeadline() {
        watchdog_.request_stop();
        changed_.notify_all();
    }

    [[nodiscard]] base::CancellationToken token() const noexcept {
        return cancellation_.get_token();
    }

  private:
    base::CancellationSource cancellation_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::jthread watchdog_;
};

base::Result<std::string> run_required(VirtualBoxCommandRunner& runner,
                                       const std::vector<std::string>& arguments,
                                       const std::string_view user_home, const char* message_code,
                                       const base::CancellationToken cancellation) {
    auto result = runner.run(arguments, user_home, cancellation);
    if (!result) {
        if (result.error().code == base::ErrorCode::kCancelled) {
            return base::Result<std::string>::failure(result.error());
        }
        return base::Result<std::string>::failure(
            stable_error(base::ErrorCode::kIoFailure, message_code));
    }
    if (result.value().exit_code != 0) {
        return base::Result<std::string>::failure(
            stable_error(base::ErrorCode::kIoFailure, message_code));
    }
    return base::Result<std::string>::success(std::move(result.value().output));
}

bool parse_supported_version(const std::string_view output, std::string& version) {
    const std::size_t end = output.find_first_of("\r\n ");
    version.assign(output.substr(0, end));
    const std::size_t first_dot = version.find('.');
    if (first_dot == std::string::npos) {
        return false;
    }
    const std::size_t second_dot = version.find('.', first_dot + 1);
    if (second_dot == std::string::npos) {
        return false;
    }
    unsigned major = 0;
    unsigned minor = 0;
    const auto major_result = std::from_chars(version.data(), version.data() + first_dot, major);
    const auto minor_result =
        std::from_chars(version.data() + first_dot + 1, version.data() + second_dot, minor);
    return major_result.ec == std::errc{} && major_result.ptr == version.data() + first_dot &&
           minor_result.ec == std::errc{} && minor_result.ptr == version.data() + second_dot &&
           major == 7 && (minor == 1 || minor == 2);
}

BootCheckVmState parse_vm_state(const std::string_view output) {
    const std::string_view key = "VMState=\"";
    const std::size_t begin = output.find(key);
    if (begin == std::string_view::npos) {
        return BootCheckVmState::kUnknown;
    }
    const std::size_t value_begin = begin + key.size();
    const std::size_t end = output.find('"', value_begin);
    const std::string_view value = output.substr(value_begin, end - value_begin);
    if (value == "running") {
        return BootCheckVmState::kRunning;
    }
    if (value == "paused") {
        return BootCheckVmState::kPaused;
    }
    if (value == "poweroff" || value == "saved") {
        return BootCheckVmState::kPoweredOff;
    }
    if (value == "aborted") {
        return BootCheckVmState::kAborted;
    }
    return BootCheckVmState::kUnknown;
}

base::Result<void> wait_for_power_off(VirtualBoxCommandRunner& runner,
                                      const std::string_view user_home, const std::string& vm_name,
                                      const char* message_code,
                                      const base::CancellationToken cancellation) {
    constexpr unsigned kMaximumPolls = 100;
    for (unsigned poll = 0; poll < kMaximumPolls; ++poll) {
        auto output = run_required(runner, {"showvminfo", vm_name, "--machinereadable"}, user_home,
                                   message_code, cancellation);
        if (!output) {
            return base::Result<void>::failure(output.error());
        }
        const BootCheckVmState state = parse_vm_state(output.value());
        if (state == BootCheckVmState::kPoweredOff || state == BootCheckVmState::kAborted) {
            return base::Result<void>::success();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return base::Result<void>::failure(stable_error(base::ErrorCode::kIoFailure, message_code));
}

std::string next_probe_name() {
    static std::atomic_uint64_t sequence{0};
    return "Aegra-Capability-Probe-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(GetTickCount64()) + "-" +
           std::to_string(sequence.fetch_add(1, std::memory_order_relaxed));
}

bool cleanup_probe(VirtualBoxCommandRunner& runner, const std::string_view user_home,
                   const std::string& vm_name, const bool started) {
    CleanupDeadline deadline;
    if (started) {
        (void)runner.run({"controlvm", vm_name, "poweroff"}, user_home, deadline.token());
        (void)wait_for_power_off(runner, user_home, vm_name, kProviderUnavailable,
                                 deadline.token());
    }
    constexpr unsigned kMaximumAttempts = 20;
    for (unsigned attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        auto removed =
            runner.run({"unregistervm", vm_name, "--delete"}, user_home, deadline.token());
        if (removed && removed.value().exit_code == 0) {
            return true;
        }
        if (deadline.token().stop_requested()) {
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

base::Result<void> probe_headless(VirtualBoxCommandRunner& runner, const std::string_view user_home,
                                  const base::CancellationToken cancellation) {
    const std::string vm_name = next_probe_name();
    auto created =
        run_required(runner,
                     {"createvm", "--name=" + vm_name, "--platform-architecture=x86",
                      "--basefolder=" + std::string(user_home), "--ostype=Other_64", "--register"},
                     user_home, kProviderUnavailable, cancellation);
    if (!created) {
        return base::Result<void>::failure(created.error());
    }

    base::Result<void> outcome = base::Result<void>::success();
    auto modified = run_required(
        runner,
        {"modifyvm", vm_name, "--memory=1024", "--cpus=1", "--firmware=bios", "--nic1=none",
         "--audio-enabled=off", "--clipboard-mode=disabled", "--clipboard-file-transfers=disabled",
         "--drag-and-drop=disabled", "--usb-ohci=off", "--usb-ehci=off", "--usb-xhci=off",
         "--vrde=off", "--recording=off", "--boot1=none", "--boot2=none", "--boot3=none",
         "--boot4=none"},
        user_home, kProviderUnavailable, cancellation);
    if (!modified) {
        outcome = base::Result<void>::failure(modified.error());
    }

    bool started = false;
    if (outcome) {
        auto start = run_required(runner, {"startvm", vm_name, "--type=headless"}, user_home,
                                  kProviderUnavailable, cancellation);
        if (!start) {
            outcome = base::Result<void>::failure(start.error());
        } else {
            started = true;
        }
    }
    if (outcome) {
        auto state = run_required(runner, {"showvminfo", vm_name, "--machinereadable"}, user_home,
                                  kProviderUnavailable, cancellation);
        if (!state || (parse_vm_state(state.value()) != BootCheckVmState::kRunning &&
                       parse_vm_state(state.value()) != BootCheckVmState::kPaused)) {
            outcome = state ? base::Result<void>::failure(
                                  stable_error(base::ErrorCode::kIoFailure, kProviderUnavailable))
                            : base::Result<void>::failure(state.error());
        }
    }
    const bool cleaned = cleanup_probe(runner, user_home, vm_name, started);
    if (!cleaned && outcome) {
        outcome = base::Result<void>::failure(
            stable_error(base::ErrorCode::kIoFailure, kProviderUnavailable));
    }
    return outcome;
}

std::vector<std::string> secure_modify_arguments(const std::string& vm_name,
                                                 const virtualization::BootCheckVmRequest& request) {
    const std::string firmware = request.firmware == virtualization::BootFirmware::kUefi
                                     ? "--firmware=efi64"
                                     : "--firmware=bios";
    return {"modifyvm",
            vm_name,
            "--memory=" + std::to_string(request.memory_mib),
            "--cpus=" + std::to_string(request.cpu_count),
            firmware,
            "--nic1=none",
            "--audio-enabled=off",
            "--audio-in=off",
            "--audio-out=off",
            "--clipboard-mode=disabled",
            "--clipboard-file-transfers=disabled",
            "--drag-and-drop=disabled",
            "--usb-ohci=off",
            "--usb-ehci=off",
            "--usb-xhci=off",
            "--vrde=off",
            "--recording=off",
            "--boot1=disk",
            "--boot2=none",
            "--boot3=none",
            "--boot4=none"};
}

class VirtualBoxVmSession final : public virtualization::IBootCheckVmSession {
  public:
    VirtualBoxVmSession(ports::IProcessLauncher& launcher, std::string executable,
                        VirtualBoxJobLayout layout, std::string provider_version)
        : runner_(launcher, std::move(executable)), layout_(std::move(layout)) {
        info_.vm_name = layout_.vm_name;
        info_.child_medium_path = layout_.child_medium;
        info_.provider_version = std::move(provider_version);
    }

    ~VirtualBoxVmSession() override;

    [[nodiscard]] const virtualization::BootCheckVmInfo& info() const noexcept override {
        return info_;
    }

    [[nodiscard]] base::Result<void> initialize(const virtualization::BootCheckVmRequest& request,
                                                base::CancellationToken cancellation);
    [[nodiscard]] base::Result<void> start(base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<BootCheckVmState>
    state(base::CancellationToken cancellation) override;
    // No agentless guest channel without Guest Additions; the caller falls back
    // to differencing-overlay growth.
    [[nodiscard]] base::Result<bool>
    guest_heartbeat_ok(base::CancellationToken) override {
        return base::Result<bool>::success(false);
    }
    [[nodiscard]] base::Result<void> power_off(base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> cleanup(base::CancellationToken cancellation) override;

  private:
    [[nodiscard]] base::Result<void> command(const std::vector<std::string>& arguments,
                                             const char* message,
                                             base::CancellationToken cancellation);
    void cleanup_step(bool& active, const std::vector<std::string>& arguments, bool& failed,
                      base::CancellationToken cancellation);

    VirtualBoxCommandRunner runner_;
    VirtualBoxJobLayout layout_;
    virtualization::BootCheckVmInfo info_;
    bool parent_registered_{false};
    bool child_registered_{false};
    bool vm_registered_{false};
    bool medium_attached_{false};
};

VirtualBoxVmSession::~VirtualBoxVmSession() {
    CleanupDeadline deadline;
    (void)cleanup(deadline.token());
}

base::Result<void> VirtualBoxVmSession::command(const std::vector<std::string>& arguments,
                                                const char* message,
                                                const base::CancellationToken cancellation) {
    auto result = run_required(runner_, arguments, layout_.user_home, message, cancellation);
    if (!result) {
        return base::Result<void>::failure(result.error());
    }
    return base::Result<void>::success();
}

base::Result<void>
VirtualBoxVmSession::initialize(const virtualization::BootCheckVmRequest& request,
                                const base::CancellationToken cancellation) {
    auto parent = command({"modifymedium", "disk", layout_.parent_vmdk, "--type=immutable"},
                          kVmCreateFailed, cancellation);
    if (!parent) {
        return parent;
    }
    parent_registered_ = true;

    auto child = command({"createmedium", "disk", "--filename=" + layout_.child_medium,
                          "--format=VDI", "--diffparent=" + layout_.parent_vmdk},
                         kVmCreateFailed, cancellation);
    if (!child) {
        return child;
    }
    child_registered_ = true;

    auto vm = command({"createvm", "--name=" + layout_.vm_name, "--platform-architecture=x86",
                       "--basefolder=" + layout_.vm_base_directory, "--ostype=Windows10_64",
                       "--register"},
                      kVmCreateFailed, cancellation);
    if (!vm) {
        return vm;
    }
    vm_registered_ = true;

    auto modified =
        command(secure_modify_arguments(layout_.vm_name, request), kVmCreateFailed, cancellation);
    if (!modified) {
        return modified;
    }
    auto controller = command({"storagectl", layout_.vm_name, "--name=SATA", "--add=sata",
                               "--controller=IntelAhci", "--portcount=1", "--bootable=on"},
                              kVmCreateFailed, cancellation);
    if (!controller) {
        return controller;
    }
    auto attached = command({"storageattach", layout_.vm_name, "--storagectl=SATA", "--port=0",
                             "--device=0", "--type=hdd", "--medium=" + layout_.child_medium},
                            kVmCreateFailed, cancellation);
    if (attached) {
        medium_attached_ = true;
    }
    return attached;
}

base::Result<void> VirtualBoxVmSession::start(const base::CancellationToken cancellation) {
    if (!vm_registered_) {
        return base::Result<void>::failure(
            stable_error(base::ErrorCode::kConflict, kVmStartFailed));
    }
    return command({"startvm", layout_.vm_name, "--type=headless"}, kVmStartFailed, cancellation);
}

base::Result<BootCheckVmState>
VirtualBoxVmSession::state(const base::CancellationToken cancellation) {
    if (!vm_registered_) {
        return base::Result<BootCheckVmState>::success(BootCheckVmState::kPoweredOff);
    }
    auto output = run_required(runner_, {"showvminfo", layout_.vm_name, "--machinereadable"},
                               layout_.user_home, kVmStartFailed, cancellation);
    if (!output) {
        return base::Result<BootCheckVmState>::failure(output.error());
    }
    return base::Result<BootCheckVmState>::success(parse_vm_state(output.value()));
}

base::Result<void> VirtualBoxVmSession::power_off(const base::CancellationToken cancellation) {
    auto current = state(cancellation);
    if (!current) {
        return base::Result<void>::failure(current.error());
    }
    if (current.value() != BootCheckVmState::kRunning &&
        current.value() != BootCheckVmState::kPaused) {
        return base::Result<void>::success();
    }
    auto stopped =
        command({"controlvm", layout_.vm_name, "poweroff"}, kCleanupIncomplete, cancellation);
    if (!stopped) {
        return stopped;
    }
    return wait_for_power_off(runner_, layout_.user_home, layout_.vm_name, kCleanupIncomplete,
                              cancellation);
}

void VirtualBoxVmSession::cleanup_step(bool& active, const std::vector<std::string>& arguments,
                                       bool& failed, const base::CancellationToken cancellation) {
    if (!active) {
        return;
    }
    constexpr unsigned kMaximumAttempts = 20;
    for (unsigned attempt = 0; attempt < kMaximumAttempts; ++attempt) {
        auto result = command(arguments, kCleanupIncomplete, cancellation);
        if (result) {
            active = false;
            return;
        }
        if (cancellation.stop_requested()) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    failed = true;
}

base::Result<void> VirtualBoxVmSession::cleanup(const base::CancellationToken cancellation) {
    bool failed = false;
    auto stopped = power_off(cancellation);
    if (!stopped) {
        failed = true;
    }
    cleanup_step(medium_attached_,
                 {"storageattach", layout_.vm_name, "--storagectl=SATA", "--port=0", "--device=0",
                  "--type=hdd", "--medium=none"},
                 failed, cancellation);
    cleanup_step(vm_registered_, {"unregistervm", layout_.vm_name}, failed, cancellation);
    cleanup_step(child_registered_, {"closemedium", "disk", layout_.child_medium, "--delete"},
                 failed, cancellation);
    cleanup_step(parent_registered_, {"closemedium", "disk", layout_.parent_vmdk}, failed,
                 cancellation);
    if (failed) {
        return base::Result<void>::failure(
            stable_error(base::ErrorCode::kIoFailure, kCleanupIncomplete));
    }
    return base::Result<void>::success();
}

} // namespace

struct VirtualBoxBootCheckProvider::Impl final {
    Impl(ports::IProcessLauncher& process_launcher, VirtualBoxProviderOptions value)
        : launcher(&process_launcher), options(std::move(value)),
          runner(process_launcher, options.vbox_manage_path) {}

    ports::IProcessLauncher* launcher{nullptr};
    VirtualBoxProviderOptions options;
    VirtualBoxCommandRunner runner;
    std::string version;
};

VirtualBoxBootCheckProvider::VirtualBoxBootCheckProvider(ports::IProcessLauncher& launcher,
                                                         VirtualBoxProviderOptions options)
    : impl_(std::make_unique<Impl>(launcher, std::move(options))) {}

VirtualBoxBootCheckProvider::~VirtualBoxBootCheckProvider() = default;

base::Result<virtualization::BootCheckProviderInfo>
VirtualBoxBootCheckProvider::inspect(const base::CancellationToken cancellation) {
    virtualization::BootCheckProviderInfo info;
    info.provider_name = "Oracle VirtualBox";
    info.message_code = kProviderUnavailable;
    if (!detail::is_trusted_vbox_manage(impl_->options.vbox_manage_path)) {
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    auto home = detail::prepare_capability_user_home(impl_->options.capability_user_home);
    if (!home) {
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    auto version_output = run_required(impl_->runner, {"--version"}, home.value(),
                                       kProviderUnavailable, cancellation);
    if (!version_output) {
        if (version_output.error().code == base::ErrorCode::kCancelled) {
            return base::Result<virtualization::BootCheckProviderInfo>::failure(
                version_output.error());
        }
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    if (!parse_supported_version(version_output.value(), impl_->version)) {
        return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    auto host = run_required(impl_->runner, {"list", "hostinfo"}, home.value(),
                             kProviderUnavailable, cancellation);
    if (!host) {
        return host.error().code == base::ErrorCode::kCancelled
                   ? base::Result<virtualization::BootCheckProviderInfo>::failure(host.error())
                   : base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    auto probe = probe_headless(impl_->runner, home.value(), cancellation);
    if (!probe) {
        return probe.error().code == base::ErrorCode::kCancelled
                   ? base::Result<virtualization::BootCheckProviderInfo>::failure(probe.error())
                   : base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
    }
    info.available = true;
    info.provider_version = impl_->version;
    info.message_code.clear();
    return base::Result<virtualization::BootCheckProviderInfo>::success(std::move(info));
}

base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>
VirtualBoxBootCheckProvider::create(const virtualization::BootCheckVmRequest& request,
                                    const base::CancellationToken cancellation) {
    auto provider = inspect(cancellation);
    if (!provider) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            provider.error());
    }
    if (!provider.value().available) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            stable_error(base::ErrorCode::kNotFound, kProviderUnavailable));
    }
    auto layout = detail::prepare_job_layout(request);
    if (!layout) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            layout.error());
    }
    auto session =
        std::make_unique<VirtualBoxVmSession>(*impl_->launcher, impl_->options.vbox_manage_path,
                                              std::move(layout.value()), impl_->version);
    auto initialized = session->initialize(request, cancellation);
    if (!initialized) {
        return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::failure(
            initialized.error());
    }
    return base::Result<std::unique_ptr<virtualization::IBootCheckVmSession>>::success(
        std::move(session));
}

} // namespace aegra::adapters::virtualbox
