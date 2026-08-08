#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"

namespace aegra::apps::worker {

// Schema 4 file_set Full backup: one VSS Snapshot Set, WindowsFileSnapshotView, FileSet pipeline,
// and PersonalFileArchiveSession. Contract validation failures reject the request; accepted task
// failures return a validated TaskResult with stable, non-sensitive message codes.
[[nodiscard]] base::Result<contracts::TaskResult> execute_windows_file_set_backup_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
