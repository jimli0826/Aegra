#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/worker_response.h"

#include <cstdint>

namespace aegra::apps::worker {

enum class WorkerExitCode : std::int32_t {
    kSucceeded = 0,
    kTaskFailed = 10,
    kCancelled = 11,
    kRequestRejected = 20,
    kHostFailure = 21,
};

struct WorkerHostResult final {
    WorkerExitCode exit_code{WorkerExitCode::kHostFailure};
    contracts::WorkerResponse response;
};

// Runs one synchronous task. The host combines process cancellation with the job deadline and
// converts every boundary failure into a validated, non-sensitive response and stable exit code.
[[nodiscard]] WorkerHostResult run_windows_personal_backup_worker_host(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
