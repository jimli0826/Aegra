#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/ntfs_boot_commit.h"
#include "aegra/ntfs_resize/ntfs_chkdsk_runner.h"
#include "aegra/ntfs_resize/shrink_plan.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstdint>
#include <string>

namespace aegra::ntfs_resize {

enum class ShrinkFinalizePhase : std::uint8_t {
    kFlushing = 1,
    kReadbackCritical = 2,
    kCommittingBackupBoot = 3,
    kCommittingPrimaryBoot = 4,
    kRunningChkdsk = 5,
    kPostchecking = 6,
    kCompleted = 7,
};

enum class ShrinkFinalizeOutcome : std::uint8_t {
    kReadyForPostcheck = 1,
    kCompleted = 2,
    kTargetIncomplete = 3,
    kCommitOutcomeUnknown = 4,
    kPostcheckFailed = 5,
    kFailed = 6,
};

struct ShrinkVolumePostcheckSnapshot final {
    bool query_succeeded{false};
    bool volume_dirty{true};
    std::uint64_t capacity_bytes{0};
    std::uint32_t bytes_per_sector{0};
};

/// Worker-provided Windows volume status probe (ports-style; no Windows types).
class IShrinkVolumePostcheck {
  public:
    virtual ~IShrinkVolumePostcheck() = default;
    [[nodiscard]] virtual base::Result<ShrinkVolumePostcheckSnapshot>
    query(base::CancellationToken cancellation) = 0;
};

struct ShrinkFinalizeRequest final {
    const ShrinkPlan* plan{nullptr};
    ports::IRandomAccessBlockDevice* target{nullptr};
    CompositeNtfsBlockDevice* composite{nullptr};
};

struct ShrinkPostcommitRequest final {
    const ShrinkPlan* plan{nullptr};
    /// Optional: CHKDSK runs only when chkdsk_executable_path is non-empty, in which case
    /// process_launcher and volume_name are required too.
    ports::IProcessLauncher* process_launcher{nullptr};
    std::string chkdsk_executable_path;
    std::string volume_name;
    IShrinkVolumePostcheck* postcheck{nullptr};
};

struct ShrinkFinalizeResult final {
    ShrinkFinalizeOutcome outcome{ShrinkFinalizeOutcome::kFailed};
    ShrinkFinalizePhase reached{ShrinkFinalizePhase::kFlushing};
    std::string message_code{"restore.shrink_target_incomplete"};
    std::string failure_detail; // diagnostic context for failed outcomes; not a message code
    ChkdskRunResult chkdsk{};
    BootCommitResult boot{};
};

/// Post-relocation finalize split at the raw target handle lifetime boundary.
/// `execute_locked_commit` runs while the target is locked and open. The caller must destroy the
/// composite and target device before calling `execute_postcommit`, which runs CHKDSK/postcheck.
class NtfsShrinkFinalizeExecutor final {
  public:
    NtfsShrinkFinalizeExecutor() = delete;

    [[nodiscard]] static base::Result<ShrinkFinalizeResult>
    execute_locked_commit(const ShrinkFinalizeRequest& request,
                          base::CancellationToken cancellation);

    [[nodiscard]] static base::Result<ShrinkFinalizeResult>
    execute_postcommit(const ShrinkPostcommitRequest& request,
                       ShrinkFinalizeResult locked_commit,
                       base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
