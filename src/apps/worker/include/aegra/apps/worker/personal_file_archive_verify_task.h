#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"

namespace aegra::apps::worker {

/// Full file_set Verify: authenticate File Index and read every stream payload byte.
[[nodiscard]] base::Result<contracts::TaskResult> execute_personal_file_archive_verify_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
