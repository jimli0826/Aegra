#pragma once

#include "ntfs_record_class.h"
#include "ntfs_record_writer.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/composite_ntfs_block_device.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>

namespace aegra::ntfs_resize::detail {

struct BitmapCommitSummary final {
    std::uint64_t bits_set{0};
    std::uint64_t bits_cleared{0};
};

/// Sets target LCNs allocated then clears source LCNs for matching relocations.
[[nodiscard]] base::Result<BitmapCommitSummary>
commit_bitmap_for_relocations(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                              const ShrinkPlan& plan, RecordClassFilter filter,
                              base::CancellationToken cancellation);

[[nodiscard]] inline base::Result<BitmapCommitSummary>
commit_ordinary_bitmap(CompositeNtfsBlockDevice& device, MftRecordStore& mft_store,
                       const ShrinkPlan& plan, base::CancellationToken cancellation) {
    return commit_bitmap_for_relocations(device, mft_store, plan, RecordClassFilter::kOrdinary,
                                         cancellation);
}

} // namespace aegra::ntfs_resize::detail
