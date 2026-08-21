#include "ntfs_shrink_audit.h"

#include "ntfs_shrink_errors.h"

#include <algorithm>
#include <set>
#include <tuple>
#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] bool ranges_overlap(const ClusterRange& left, const ClusterRange& right) noexcept {
    return left.begin_lcn < right.end_lcn && right.begin_lcn < left.end_lcn;
}

} // namespace

base::Result<void> audit_relocation_plan(const MftScanResult& scan,
                                         const RelocationPlanResult& plan,
                                         const std::uint64_t new_total_cluster_count) {
    if (plan.relocations.size() != scan.outbound_extents.size()) {
        return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
    }

    std::vector<ClusterRange> targets;
    targets.reserve(plan.relocations.size());
    for (const auto& record : plan.relocations) {
        if (record.cluster_count == 0 || record.source.end_lcn <= record.source.begin_lcn ||
            record.target.end_lcn <= record.target.begin_lcn ||
            record.target.end_lcn - record.target.begin_lcn != record.cluster_count ||
            record.source.end_lcn - record.source.begin_lcn != record.cluster_count) {
            return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
        }
        if (record.target.begin_lcn < 1 || record.target.end_lcn > new_total_cluster_count) {
            return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
        }
        if (record.source.begin_lcn < new_total_cluster_count) {
            return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
        }
        for (const auto& prior : targets) {
            if (ranges_overlap(prior, record.target)) {
                return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
            }
        }
        targets.push_back(record.target);
    }

    std::set<std::tuple<std::uint64_t, std::uint32_t, std::u16string, std::uint16_t>> mutated;
    for (const auto& mutation : plan.mutations) {
        mutated.emplace(mutation.mft_record_number, mutation.attribute_type, mutation.attribute_name,
                        mutation.attribute_id);
    }
    for (const auto& owned : scan.attributes_with_outbound) {
        if (!mutated.contains({owned.mft_record_number, owned.attribute_type, owned.attribute_name,
                               owned.attribute_id})) {
            return shrink_fail_void(base::ErrorCode::kInternal, "restore.shrink_plan_corrupt");
        }
    }
    return base::Result<void>::success();
}

} // namespace aegra::ntfs_resize::detail
