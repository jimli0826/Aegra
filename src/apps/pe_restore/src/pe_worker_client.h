#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/random.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

namespace aegra::apps::pe_restore {

struct PeWorkerProgress final {
    std::uint64_t processed_bytes{0};
    std::optional<std::uint64_t> logical_bytes;
    std::string message_code;
};

struct PeWorkerOutcome final {
    contracts::TaskOutcome outcome{contracts::TaskOutcome::kFailed};
    base::ErrorCode error_code{base::ErrorCode::kNone};
    std::string message_code;
};

struct PeWorkerSessionRequest final {
    contracts::JobRequest job;
    /// Absolute path of aegra_personal_worker.exe (injected beside this executable).
    std::string worker_executable_path_utf8;
    ports::IProcessLauncher* launcher{nullptr};
    ports::IRandomSource* random{nullptr};
    /// Called on the session thread for every progress event; must not block long.
    std::function<void(const PeWorkerProgress&)> on_progress;
};

/// Runs one restore job through aegra_personal_worker.exe over the worker session
/// protocol (ADR-0008), exactly like the service supervisor: listen on a random
/// named pipe, spawn `--pipe <name>`, send the job, forward progress events, and
/// return the terminal outcome. Blocking; returns after the worker process exits.
[[nodiscard]] base::Result<PeWorkerOutcome>
run_pe_worker_session(const PeWorkerSessionRequest& request,
                      base::CancellationToken cancellation);

} // namespace aegra::apps::pe_restore
