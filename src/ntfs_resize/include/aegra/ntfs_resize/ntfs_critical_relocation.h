#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>

namespace aegra::ntfs_resize {

struct CriticalRelocationProgress final {
    std::uint64_t verified_moved_bytes{0};
    std::uint64_t completed_relocations{0};
    std::uint64_t total_critical_relocations{0};
    bool logfile_invalidated{false};
    bool mft_mirror_synced{false};
};

class ICriticalRelocationProgressSink {
  public:
    virtual ~ICriticalRelocationProgressSink() = default;
    virtual void on_progress(const CriticalRelocationProgress& progress) noexcept = 0;
};

struct CriticalRelocationRequest final {
    const ShrinkPlan* plan{nullptr};
    CompositeNtfsBlockDevice* device{nullptr};
    ICriticalRelocationProgressSink* progress{nullptr};
};

struct CriticalRelocationSummary final {
    std::uint64_t verified_moved_bytes{0};
    std::uint64_t relocation_count{0};
    std::uint64_t records_updated{0};
    std::uint64_t bitmap_bits_set{0};
    std::uint64_t bitmap_bits_cleared{0};
    bool logfile_invalidated{false};
    bool mft_mirror_synced{false};
};

/// Relocates critical system-file clusters (MFT records 0..11), updates runlists/$Bitmap,
/// invalidates $LogFile restart area, and syncs $MFTMirr. Does not commit Boot (SR8).
class NtfsCriticalRelocationExecutor final {
  public:
    NtfsCriticalRelocationExecutor() = delete;

    [[nodiscard]] static base::Result<CriticalRelocationSummary>
    execute(const CriticalRelocationRequest& request, base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
