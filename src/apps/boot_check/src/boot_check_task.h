#pragma once

#include "aegra/apps/boot_check/boot_check_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/boot_check_job.h"
#include "aegra/contracts/task_result.h"

#include <chrono>

namespace aegra::apps::boot_check::detail {

/// Runs one accepted BootCheck job through the fixed stage pipeline and always
/// returns a validated TaskResult; Result failure is reserved for internal
/// contract errors that the host maps to a host failure.
[[nodiscard]] base::Result<contracts::TaskResult>
run_boot_check_task(const contracts::BootCheckJobRequest& request,
                    const BootCheckHostOptions& options, const BootCheckHostContext& context,
                    const base::CancellationToken& cancellation);

/// Diagnostic: runs the pipeline up to present_vmdk and keeps the read-only
/// VMDK mounted for hold_duration (or until cancelled) so an external
/// hypervisor can open it, then cleans up. Never creates a VM.
[[nodiscard]] base::Result<contracts::TaskResult>
run_boot_check_present_hold(const contracts::BootCheckJobRequest& request,
                            const BootCheckHostOptions& options,
                            const BootCheckHostContext& context, std::chrono::minutes hold_duration,
                            const base::CancellationToken& cancellation);

} // namespace aegra::apps::boot_check::detail
