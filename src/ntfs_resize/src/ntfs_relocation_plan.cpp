#include "ntfs_relocation_plan.h"

#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"
#include "shrink_plan_internal.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/runlist.h"

#include <map>
#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

struct ExtentKey final {
    std::uint64_t mft_record_number{0};
    std::uint32_t attribute_type{0};
    std::u16string attribute_name;
    std::uint16_t attribute_id{0};
    std::uint64_t first_vcn{0};

    [[nodiscard]] bool operator<(const ExtentKey& other) const noexcept {
        if (mft_record_number != other.mft_record_number) {
            return mft_record_number < other.mft_record_number;
        }
        if (attribute_type != other.attribute_type) {
            return attribute_type < other.attribute_type;
        }
        if (attribute_name != other.attribute_name) {
            return attribute_name < other.attribute_name;
        }
        if (attribute_id != other.attribute_id) {
            return attribute_id < other.attribute_id;
        }
        return first_vcn < other.first_vcn;
    }
};

[[nodiscard]] ExtentKey make_extent_key(const OutboundExtent& extent) {
    return ExtentKey{extent.mft_record_number, extent.attribute_type, extent.attribute_name,
                     extent.attribute_id, extent.run.first_vcn.value};
}

[[nodiscard]] base::Result<std::vector<ntfs_core::DataRun>>
rebuild_runs_for_attribute(const OwnedAttributeRuns& owned,
                           const std::map<ExtentKey, ClusterRange>& targets,
                           const std::uint64_t new_total_cluster_count) {
    std::vector<ntfs_core::DataRun> rebuilt;
    rebuilt.reserve(owned.runs.size() * 2U);
    for (const auto& run : owned.runs) {
        if (run.sparse || run.cluster_count.value == 0) {
            rebuilt.push_back(run);
            continue;
        }
        std::uint64_t end_lcn = 0;
        if (!ntfs_core::checked_add_u64(run.first_lcn.value, run.cluster_count.value, end_lcn)) {
            return shrink_fail<std::vector<ntfs_core::DataRun>>(
                base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
        }
        if (end_lcn <= new_total_cluster_count) {
            rebuilt.push_back(run);
            continue;
        }

        if (run.first_lcn.value < new_total_cluster_count) {
            ntfs_core::DataRun inside = run;
            inside.cluster_count.value = new_total_cluster_count - run.first_lcn.value;
            rebuilt.push_back(inside);
        }

        const auto outbound_begin_lcn = (std::max)(run.first_lcn.value, new_total_cluster_count);
        const auto skipped = outbound_begin_lcn - run.first_lcn.value;
        ExtentKey key{owned.mft_record_number, owned.attribute_type, owned.attribute_name,
                      owned.attribute_id, run.first_vcn.value + skipped};
        const auto found = targets.find(key);
        if (found == targets.end()) {
            return shrink_fail<std::vector<ntfs_core::DataRun>>(base::ErrorCode::kInternal,
                                                                "restore.shrink_plan_corrupt");
        }
        const auto outbound_count = end_lcn - outbound_begin_lcn;
        if (found->second.end_lcn - found->second.begin_lcn != outbound_count) {
            return shrink_fail<std::vector<ntfs_core::DataRun>>(base::ErrorCode::kInternal,
                                                                "restore.shrink_plan_corrupt");
        }
        ntfs_core::DataRun moved;
        moved.first_vcn.value = key.first_vcn;
        moved.first_lcn.value = found->second.begin_lcn;
        moved.cluster_count.value = outbound_count;
        moved.sparse = false;
        rebuilt.push_back(moved);
    }

    auto valid = ntfs_core::validate_data_runs(rebuilt);
    if (!valid) {
        return base::Result<std::vector<ntfs_core::DataRun>>::failure(valid.error());
    }
    auto encoded = ntfs_core::encode_runlist_bounded(rebuilt, owned.runlist_capacity_bytes);
    if (!encoded) {
        return shrink_fail<std::vector<ntfs_core::DataRun>>(base::ErrorCode::kUnsupportedVersion,
                                                            "restore.shrink_unsupported_layout");
    }
    return base::Result<std::vector<ntfs_core::DataRun>>::success(std::move(rebuilt));
}

void append_critical_ops(const MftScanResult& scan, RelocationPlanResult& plan) {
    std::uint32_t order = 0;
    plan.critical_operations.push_back(
        CriticalFileOperation{kFileNumberLogFile, CriticalFileOperationKind::kInvalidateLogfile,
                              order++});
    plan.critical_operations.push_back(
        CriticalFileOperation{kFileNumberBitmap, CriticalFileOperationKind::kUpdateBitmap, order++});
    plan.critical_operations.push_back(CriticalFileOperation{
        kFileNumberVolume, CriticalFileOperationKind::kUpdateVolumeSize, order++});
    plan.critical_operations.push_back(
        CriticalFileOperation{kFileNumberBoot, CriticalFileOperationKind::kUpdateBootGeometry,
                              order++});

    bool mft_outbound = false;
    for (const auto& extent : scan.outbound_extents) {
        if (extent.mft_record_number == kFileNumberMft) {
            mft_outbound = true;
            break;
        }
    }
    if (mft_outbound) {
        plan.critical_operations.push_back(
            CriticalFileOperation{kFileNumberMft, CriticalFileOperationKind::kRelocateMft, order++});
    }
}

} // namespace

base::Result<RelocationPlanResult>
build_relocation_plan(const MftScanResult& scan, BitmapAllocator& allocator,
                      const std::uint64_t new_total_cluster_count) {
    RelocationPlanResult plan;
    plan.relocations.reserve(scan.outbound_extents.size());
    std::map<ExtentKey, ClusterRange> targets;
    std::uint32_t order = 0;
    for (const auto& extent : scan.outbound_extents) {
        auto target = allocate_first_fit(allocator, extent.run.cluster_count.value);
        if (!target) {
            return base::Result<RelocationPlanResult>::failure(target.error());
        }
        RelocationRecord record;
        record.source.begin_lcn = extent.run.first_lcn.value;
        record.source.end_lcn = extent.run.first_lcn.value + extent.run.cluster_count.value;
        record.target = target.value();
        record.cluster_count = extent.run.cluster_count.value;
        record.mft_record_number = extent.mft_record_number;
        record.attribute_type = extent.attribute_type;
        record.attribute_name = extent.attribute_name;
        record.attribute_id = extent.attribute_id;
        record.plan_order = order++;
        plan.relocation_cluster_count += record.cluster_count;
        targets.emplace(make_extent_key(extent), target.value());
        plan.relocations.push_back(std::move(record));
    }

    for (const auto& owned : scan.attributes_with_outbound) {
        auto rebuilt = rebuild_runs_for_attribute(owned, targets, new_total_cluster_count);
        if (!rebuilt) {
            return base::Result<RelocationPlanResult>::failure(rebuilt.error());
        }
        MetadataMutation mutation;
        mutation.mft_record_number = owned.mft_record_number;
        mutation.expected_record_sequence = owned.record_sequence;
        mutation.attribute_type = owned.attribute_type;
        mutation.attribute_name = owned.attribute_name;
        mutation.attribute_id = owned.attribute_id;
        mutation.attribute_record_offset = owned.attribute_record_offset;
        mutation.attribute_length = owned.attribute_length;
        mutation.runlist_offset = owned.runlist_offset;
        mutation.kind = MetadataMutationKind::kRunlistReplace;
        auto preimage = ntfs_core::encode_runlist_bounded(owned.runs,
                                                          owned.runlist_capacity_bytes);
        auto replacement = ntfs_core::encode_runlist_bounded(rebuilt.value(),
                                                              owned.runlist_capacity_bytes);
        if (!preimage || !replacement) {
            return shrink_fail<RelocationPlanResult>(base::ErrorCode::kUnsupportedVersion,
                                                     "restore.shrink_unsupported_layout");
        }
        mutation.preimage_digest = digest_to_hex(sha256(preimage.value()));
        mutation.replacement_runlist = std::move(replacement).value();
        plan.mutations.push_back(std::move(mutation));
    }

    append_critical_ops(scan, plan);
    return base::Result<RelocationPlanResult>::success(std::move(plan));
}

} // namespace aegra::ntfs_resize::detail
