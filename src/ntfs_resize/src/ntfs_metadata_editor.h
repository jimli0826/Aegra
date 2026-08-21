#pragma once

#include "ntfs_record_class.h"
#include "ntfs_record_writer.h"

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"

#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ntfs_resize::detail {

/// Applies runlist replacements for matching metadata mutations and writes MFT records.
[[nodiscard]] base::Result<std::uint64_t>
apply_runlist_mutations(MftRecordStore& store, const ShrinkPlan& plan,
                        std::uint64_t new_total_cluster_count, RecordClassFilter filter,
                        base::CancellationToken cancellation);

[[nodiscard]] inline base::Result<std::uint64_t>
apply_ordinary_runlist_mutations(MftRecordStore& store, const ShrinkPlan& plan,
                                 std::uint64_t new_total_cluster_count,
                                 base::CancellationToken cancellation) {
    return apply_runlist_mutations(store, plan, new_total_cluster_count, RecordClassFilter::kOrdinary,
                                   cancellation);
}

} // namespace aegra::ntfs_resize::detail
