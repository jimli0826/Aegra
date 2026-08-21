#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ntfs_resize {

struct PrecommitAuditFinding final {
    std::string code; // stable message code; empty on unused slots
};

struct PrecommitAuditReport final {
    bool passed{false};
    std::vector<std::string> failure_codes;   // non-empty iff !passed
    std::vector<std::string> failure_details; // parallel to failure_codes; may hold empty entries
};

struct PrecommitAuditRequest final {
    const ShrinkPlan* plan{nullptr};
    CompositeNtfsBlockDevice* device{nullptr};
    ports::IRandomAccessBlockDevice* target{nullptr};
};

/// Structural auditor run after ordinary+critical relocation and before Boot commit. Final MFT,
/// Bitmap and mirror reads are performed only from the real target device; Archive/Scratch cannot
/// satisfy a missing target write.
/// Returns a single pass/fail; warnings are not used as success substitutes.
class NtfsPrecommitAuditor final {
  public:
    NtfsPrecommitAuditor() = delete;

    [[nodiscard]] static base::Result<PrecommitAuditReport>
    audit(const PrecommitAuditRequest& request, base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
