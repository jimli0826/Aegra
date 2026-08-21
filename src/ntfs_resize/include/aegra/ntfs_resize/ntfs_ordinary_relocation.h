#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>

namespace aegra::ntfs_resize {

struct OrdinaryRelocationProgress final {
    std::uint64_t verified_moved_bytes{0};
    std::uint64_t completed_relocations{0};
    std::uint64_t total_ordinary_relocations{0};
};

/// Optional progress sink; implementations must not throw across the boundary.
class IOrdinaryRelocationProgressSink {
  public:
    virtual ~IOrdinaryRelocationProgressSink() = default;
    virtual void on_progress(const OrdinaryRelocationProgress& progress) noexcept = 0;
};

struct OrdinaryRelocationRequest final {
    const ShrinkPlan* plan{nullptr};
    CompositeNtfsBlockDevice* device{nullptr};
    IOrdinaryRelocationProgressSink* progress{nullptr};
};

struct OrdinaryRelocationSummary final {
    std::uint64_t verified_moved_bytes{0};
    std::uint64_t relocation_count{0};
    std::uint64_t records_updated{0};
    std::uint64_t bitmap_bits_set{0};
    std::uint64_t bitmap_bits_cleared{0};
};

/// Relocates ordinary-file clusters (MFT record numbers > 11), updates their runlists and $Bitmap.
/// Does not relocate critical system files and never commits Boot Sector (SR7/SR8).
class NtfsOrdinaryRelocationExecutor final {
  public:
    NtfsOrdinaryRelocationExecutor() = delete;

    [[nodiscard]] static base::Result<OrdinaryRelocationSummary>
    execute(const OrdinaryRelocationRequest& request, base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
