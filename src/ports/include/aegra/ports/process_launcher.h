#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ports {

/// Result of a successful process launch.
struct ProcessLaunchResult final {
    std::uint32_t pid{};
};

/// Specification for launching a new process.
struct ProcessLaunchRequest final {
    std::string executable_path;
    std::vector<std::string> arguments;
};

/// Status returned when a process exits.
struct ProcessExitStatus final {
    std::uint32_t exit_code{};
    /// True when the process was forcefully terminated rather than exiting normally.
    bool terminated{false};
};

/// Abstraction for creating, waiting on, and terminating child processes.
///
/// Implementations must manage platform-specific handles internally.  Callers
/// identify processes by PID only.  Each launched process must eventually be
/// waited on or terminated to avoid resource leaks.
///
/// Thread safety: implementations support one waiter per PID and concurrent
/// terminate for that same PID. Multiple concurrent waiters for one PID are invalid.
class IProcessLauncher {
  public:
    IProcessLauncher() = default;
    virtual ~IProcessLauncher() = default;
    IProcessLauncher(const IProcessLauncher&) = delete;
    IProcessLauncher& operator=(const IProcessLauncher&) = delete;
    IProcessLauncher(IProcessLauncher&&) = delete;
    IProcessLauncher& operator=(IProcessLauncher&&) = delete;

    /// Launch a new process.  Returns the PID on success.
    [[nodiscard]] virtual base::Result<ProcessLaunchResult>
    launch(const ProcessLaunchRequest& request) = 0;

    /// Block until the process exits or cancellation is requested.
    /// Returns the exit status on normal completion.  Returns kCancelled
    /// when the cancellation token fires (the process is NOT terminated
    /// automatically; the caller must decide whether to terminate).
    [[nodiscard]] virtual base::Result<ProcessExitStatus>
    wait(std::uint32_t pid, const base::CancellationToken& cancellation) = 0;

    /// Forcefully terminate a process.  Idempotent: succeeds silently if
    /// the process has already exited.
    [[nodiscard]] virtual base::Result<void> terminate(std::uint32_t pid) = 0;
};

} // namespace aegra::ports
