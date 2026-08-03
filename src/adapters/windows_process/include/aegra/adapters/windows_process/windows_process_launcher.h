#pragma once

#include "aegra/ports/process_launcher.h"
#include <memory>

namespace aegra::adapters::windows_process {

class WindowsProcessLauncher final : public ports::IProcessLauncher {
  public:
    WindowsProcessLauncher();
    ~WindowsProcessLauncher() override;

    WindowsProcessLauncher(const WindowsProcessLauncher&) = delete;
    WindowsProcessLauncher& operator=(const WindowsProcessLauncher&) = delete;
    WindowsProcessLauncher(WindowsProcessLauncher&&) = delete;
    WindowsProcessLauncher& operator=(WindowsProcessLauncher&&) = delete;

    [[nodiscard]] base::Result<ports::ProcessLaunchResult>
    launch(const ports::ProcessLaunchRequest& request) override;

    [[nodiscard]] base::Result<ports::ProcessExitStatus>
    wait(std::uint32_t pid, const base::CancellationToken& cancellation) override;

    [[nodiscard]] base::Result<void> terminate(std::uint32_t pid) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::windows_process
