#include "virtualbox_command_runner.h"

#include <utility>

namespace aegra::adapters::virtualbox::detail {

VirtualBoxCommandRunner::VirtualBoxCommandRunner(ports::IProcessLauncher& launcher,
                                                 std::string executable_path)
    : launcher_(&launcher), executable_path_(std::move(executable_path)) {}

base::Result<VirtualBoxCommandResult>
VirtualBoxCommandRunner::run(const std::vector<std::string>& arguments,
                             const std::string_view user_home,
                             const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<VirtualBoxCommandResult>::failure(
            {base::ErrorCode::kCancelled, "bootcheck.cancelled"});
    }

    ports::ProcessLaunchRequest request;
    request.executable_path = executable_path_;
    request.arguments = arguments;
    request.capture_output = true;
    // An empty user_home means "use the invoking user's default VirtualBox
    // registry" (user-visible job path): leave VBOX_USER_HOME unset so VBoxManage
    // resolves it from the inherited user environment.
    if (!user_home.empty()) {
        request.environment_overrides.push_back(
            ports::ProcessEnvironmentVariable{"VBOX_USER_HOME", std::string(user_home)});
    }
    auto launched = launcher_->launch(request);
    if (!launched) {
        return base::Result<VirtualBoxCommandResult>::failure(launched.error());
    }

    auto waited = launcher_->wait(launched.value().pid, cancellation);
    if (!waited && waited.error().code == base::ErrorCode::kCancelled) {
        (void)launcher_->terminate(launched.value().pid);
        auto reaped = launcher_->wait(launched.value().pid, base::CancellationToken{});
        (void)reaped;
        return base::Result<VirtualBoxCommandResult>::failure(
            {base::ErrorCode::kCancelled, "bootcheck.cancelled"});
    }
    if (!waited) {
        return base::Result<VirtualBoxCommandResult>::failure(waited.error());
    }

    VirtualBoxCommandResult result;
    result.exit_code = waited.value().exit_code;
    result.output = std::move(waited.value().output);
    return base::Result<VirtualBoxCommandResult>::success(std::move(result));
}

} // namespace aegra::adapters::virtualbox::detail
