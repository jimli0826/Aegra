#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"

namespace aegra::apps::worker {

/// file_set selective restore: open V7 File Archive reader, bind Windows tree sink, run pipeline.
[[nodiscard]] base::Result<contracts::TaskResult> execute_personal_file_archive_restore_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
