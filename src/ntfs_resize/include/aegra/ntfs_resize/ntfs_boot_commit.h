#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ntfs_resize {

enum class BootCommitStage : std::uint8_t {
    kPrepared = 1,
    kBackupCommitted = 2,
    kPrimaryCommitted = 3,
};

enum class BootCommitStatus : std::uint8_t {
    kSuccess = 1,
    kTargetIncomplete = 2,
    kOutcomeUnknown = 3,
};

struct BootCommitRequest final {
    const ShrinkPlan* plan{nullptr};
    /// Target device used for destructive Boot writes (true capacity).
    ports::IRandomAccessBlockDevice* target{nullptr};
    /// Composite view used only to read the escrowed original Boot from source/archive.
    CompositeNtfsBlockDevice* composite{nullptr};
};

struct BootCommitResult final {
    BootCommitStatus status{BootCommitStatus::kTargetIncomplete};
    BootCommitStage reached{BootCommitStage::kPrepared};
    std::string message_code{"restore.shrink_target_incomplete"};
    std::vector<std::byte> committed_boot;
};

/// Commits Backup Boot then Primary Boot with flush/readback. Never rolls back with reverse MFT
/// edits. Cancel during this stage is deferred until a stable outcome is known.
class NtfsBootCommitExecutor final {
  public:
    NtfsBootCommitExecutor() = delete;

    [[nodiscard]] static base::Result<BootCommitResult>
    execute(const BootCommitRequest& request, base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
