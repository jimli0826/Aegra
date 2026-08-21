#pragma once

#include "personal_archive_restore_task_backend.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/pipeline/restore_pipeline.h"

namespace aegra::apps::worker::detail {

/// NTFS smaller-target volume restore (ADR-0025). Re-analyzes ShrinkPlan, verifies digest,
/// then runs prefix restore → relocation → precommit audit → Boot/CHKDSK finalize.
[[nodiscard]] base::Result<pipeline::RestoreSummary>
run_shrink_volume_restore(const PersonalArchiveRestoreBackendRequest& request,
                          const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker::detail
