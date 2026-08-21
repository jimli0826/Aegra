#include "aegra/ntfs_resize/ntfs_shrink_analyzer.h"

#include "ntfs_bitmap_allocator.h"
#include "ntfs_mft_scanner.h"
#include "ntfs_relocation_plan.h"
#include "ntfs_shrink_audit.h"
#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"
#include "shrink_plan_internal.h"

#include "aegra/ntfs_core/binary.h"

#include <utility>

namespace aegra::ntfs_resize {
namespace {

[[nodiscard]] std::uint64_t
resolve_boot_file_lcn(const std::uint64_t file_number, const std::uint64_t source_lcn,
                      const std::vector<RelocationRecord>& relocations) noexcept {
    constexpr std::uint32_t kDataAttributeType = 0x80U;
    for (const auto& relocation : relocations) {
        if (relocation.mft_record_number == file_number &&
            relocation.attribute_type == kDataAttributeType && relocation.attribute_name.empty() &&
            relocation.source.begin_lcn == source_lcn) {
            return relocation.target.begin_lcn;
        }
    }
    return source_lcn;
}

[[nodiscard]] base::Result<void> validate_analyze_request(const NtfsShrinkAnalyzeRequest& request) {
    if (request.source_volume == nullptr) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "ntfs_resize.plan_missing_source");
    }
    if (request.expected_source_logical_size_bytes == 0 || request.target_capacity_bytes == 0) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "restore.shrink_below_minimum");
    }
    if (request.source_chain_fingerprint.empty() || request.target_stable_id_digest.empty()) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "restore.shrink_plan_corrupt");
    }
    if (request.target_capacity_bytes >= request.expected_source_logical_size_bytes) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "restore.shrink_unsupported_layout");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_target_geometry(const ntfs_core::BootGeometry& boot,
                         const ports::BlockDeviceGeometry& target,
                         const std::uint64_t target_capacity_bytes) {
    if (target.capacity_bytes != 0 && target.capacity_bytes != target_capacity_bytes) {
        return detail::shrink_fail_void(base::ErrorCode::kConflict,
                                        "restore.shrink_sector_mismatch");
    }
    if (target.logical_sector_size != 0 && target.logical_sector_size != boot.bytes_per_sector) {
        return detail::shrink_fail_void(base::ErrorCode::kConflict,
                                        "restore.shrink_sector_mismatch");
    }
    if (boot.bytes_per_sector == 0 || boot.sectors_per_cluster == 0 ||
        boot.bytes_per_cluster == 0) {
        return detail::shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_not_ntfs");
    }
    if (target_capacity_bytes % boot.bytes_per_sector != 0) {
        return detail::shrink_fail_void(base::ErrorCode::kConflict,
                                        "restore.shrink_sector_mismatch");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::pair<std::uint64_t, std::uint64_t>>
compute_new_counts(const ntfs_core::BootGeometry& boot, const std::uint64_t target_capacity_bytes) {
    const auto device_sectors = target_capacity_bytes / boot.bytes_per_sector;
    if (device_sectors <= 1U) {
        return detail::shrink_fail<std::pair<std::uint64_t, std::uint64_t>>(
            base::ErrorCode::kInsufficientSpace, "restore.shrink_below_minimum");
    }
    // NTFS keeps the backup Boot Sector at the zero-based sector recorded by BPB
    // TotalSectors. The raw device therefore contains TotalSectors + 1 sectors.
    const auto new_total_sectors = device_sectors - 1U;
    if (new_total_sectors < boot.sectors_per_cluster * 16U) {
        return detail::shrink_fail<std::pair<std::uint64_t, std::uint64_t>>(
            base::ErrorCode::kInsufficientSpace, "restore.shrink_below_minimum");
    }
    const auto new_clusters = new_total_sectors / boot.sectors_per_cluster;
    if (new_clusters == 0 || new_clusters > boot.total_clusters) {
        return detail::shrink_fail<std::pair<std::uint64_t, std::uint64_t>>(
            base::ErrorCode::kInsufficientSpace, "restore.shrink_below_minimum");
    }
    return base::Result<std::pair<std::uint64_t, std::uint64_t>>::success(
        std::pair<std::uint64_t, std::uint64_t>{new_total_sectors, new_clusters});
}

[[nodiscard]] base::Result<std::uint64_t>
minimum_device_bytes_for_clusters(const ntfs_core::BootGeometry& boot,
                                  const std::uint64_t cluster_count) {
    std::uint64_t cluster_bytes = 0;
    std::uint64_t device_bytes = 0;
    if (!ntfs_core::checked_mul_u64(cluster_count, boot.bytes_per_cluster, cluster_bytes) ||
        !ntfs_core::checked_add_u64(cluster_bytes, boot.bytes_per_sector, device_bytes)) {
        return detail::shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                                  "restore.shrink_plan_corrupt");
    }
    return base::Result<std::uint64_t>::success(device_bytes);
}

[[nodiscard]] std::vector<ByteRange>
make_protected_ranges(const ntfs_core::BootGeometry& boot,
                      const std::uint64_t target_capacity_bytes) {
    std::vector<ByteRange> ranges;
    ranges.push_back(ByteRange{0, boot.bytes_per_sector});
    if (target_capacity_bytes >= boot.bytes_per_sector) {
        ranges.push_back(
            ByteRange{target_capacity_bytes - boot.bytes_per_sector, target_capacity_bytes});
    }
    return ranges;
}

[[nodiscard]] base::Result<std::uint64_t>
compute_scratch_upper_bound(const std::uint64_t relocation_bytes,
                            const std::size_t mutated_attribute_count) {
    constexpr std::uint64_t kPageBytes = 64U * 1024U;
    constexpr std::uint64_t kMetadataReserveBytes = 16U * 1024U * 1024U;
    std::uint64_t attribute_bytes = 0;
    std::uint64_t total = 0;
    if (!ntfs_core::checked_mul_u64(mutated_attribute_count, kPageBytes, attribute_bytes) ||
        !ntfs_core::checked_add_u64(relocation_bytes, kMetadataReserveBytes, total) ||
        !ntfs_core::checked_add_u64(total, attribute_bytes, total) ||
        !ntfs_core::checked_add_u64(total, kPageBytes - 1U, total)) {
        return detail::shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                                  "restore.shrink_plan_corrupt");
    }
    return base::Result<std::uint64_t>::success((total / kPageBytes) * kPageBytes);
}

} // namespace

base::Result<ShrinkPlan> NtfsShrinkAnalyzer::analyze(const NtfsShrinkAnalyzeRequest& request,
                                                     base::CancellationToken cancellation) {
    if (auto valid = validate_analyze_request(request); !valid) {
        return base::Result<ShrinkPlan>::failure(valid.error());
    }
    if (cancellation.stop_requested()) {
        return detail::shrink_fail<ShrinkPlan>(base::ErrorCode::kCancelled, "ntfs.read_failed");
    }

    const detail::NtfsVolumeOpenDiagnostics diagnostics{
        request.observer, request.target_capacity_bytes, request.target_geometry};
    auto view = detail::open_ntfs_volume_view(*request.source_volume,
                                              request.expected_source_logical_size_bytes,
                                              cancellation, diagnostics);
    if (!view) {
        return base::Result<ShrinkPlan>::failure(view.error());
    }
    if (auto geometry = validate_target_geometry(view.value().geometry, request.target_geometry,
                                                 request.target_capacity_bytes);
        !geometry) {
        return base::Result<ShrinkPlan>::failure(geometry.error());
    }
    auto counts = compute_new_counts(view.value().geometry, request.target_capacity_bytes);
    if (!counts) {
        return base::Result<ShrinkPlan>::failure(counts.error());
    }
    const auto new_total_sector_count = counts.value().first;
    const auto new_total_cluster_count = counts.value().second;
    if (request.observer != nullptr) {
        request.observer->candidate_counts(
            {request.target_capacity_bytes, new_total_sector_count, new_total_cluster_count});
        request.observer->candidate_stage(request.target_capacity_bytes,
                                          "validate_volume_and_logfile");
    }

    if (auto safety = detail::reject_dirty_or_unsafe_logfile(view.value(), cancellation); !safety) {
        return base::Result<ShrinkPlan>::failure(safety.error());
    }

    if (request.observer != nullptr) {
        request.observer->candidate_stage(request.target_capacity_bytes, "scan_mft_extents");
    }

    auto scan =
        detail::scan_outbound_mft_extents(view.value(), new_total_cluster_count, cancellation);
    if (!scan) {
        return base::Result<ShrinkPlan>::failure(scan.error());
    }
    if (request.observer != nullptr) {
        request.observer->candidate_stage(request.target_capacity_bytes, "load_volume_bitmap");
    }
    auto allocator =
        detail::build_bitmap_allocator(view.value(), new_total_cluster_count, cancellation);
    if (!allocator) {
        return base::Result<ShrinkPlan>::failure(allocator.error());
    }
    if (request.observer != nullptr) {
        request.observer->candidate_allocation(
            {request.target_capacity_bytes, new_total_cluster_count,
             scan.value().allocated_beyond_clusters, allocator.value().allocatable_clusters,
             allocator.value().reserved_clusters});
    }
    if (scan.value().allocated_beyond_clusters > allocator.value().allocatable_clusters ||
        scan.value().allocated_beyond_clusters >
            allocator.value().allocatable_clusters - allocator.value().reserved_clusters) {
        return detail::shrink_fail<ShrinkPlan>(base::ErrorCode::kInsufficientSpace,
                                               "restore.shrink_below_minimum");
    }

    auto relocation =
        detail::build_relocation_plan(scan.value(), allocator.value(), new_total_cluster_count);
    if (!relocation) {
        return base::Result<ShrinkPlan>::failure(relocation.error());
    }
    if (auto audited = detail::audit_relocation_plan(scan.value(), relocation.value(),
                                                     new_total_cluster_count);
        !audited) {
        return base::Result<ShrinkPlan>::failure(audited.error());
    }

    const auto cluster_size = view.value().geometry.bytes_per_cluster;
    std::uint64_t relocation_bytes = 0;
    if (!ntfs_core::checked_mul_u64(relocation.value().relocation_cluster_count, cluster_size,
                                    relocation_bytes)) {
        return detail::shrink_fail<ShrinkPlan>(base::ErrorCode::kCorruptData,
                                               "restore.shrink_plan_corrupt");
    }
    auto minimum_target_bytes =
        minimum_device_bytes_for_clusters(view.value().geometry, new_total_cluster_count);
    if (!minimum_target_bytes) {
        return base::Result<ShrinkPlan>::failure(minimum_target_bytes.error());
    }
    auto scratch_upper_bound =
        compute_scratch_upper_bound(relocation_bytes, scan.value().attributes_with_outbound.size());
    if (!scratch_upper_bound) {
        return base::Result<ShrinkPlan>::failure(scratch_upper_bound.error());
    }

    const auto new_mft_start_lcn = resolve_boot_file_lcn(
        0, view.value().geometry.mft_start_lcn.value, relocation.value().relocations);
    const auto new_mft_mirror_start_lcn = resolve_boot_file_lcn(
        1, view.value().geometry.mft_mirror_start_lcn.value, relocation.value().relocations);
    if (new_mft_start_lcn >= new_total_cluster_count ||
        new_mft_mirror_start_lcn >= new_total_cluster_count) {
        return detail::shrink_fail<ShrinkPlan>(base::ErrorCode::kUnsupportedVersion,
                                               "restore.shrink_unsupported_layout");
    }

    const auto boot_digest = detail::digest_to_hex(
        detail::sha256(std::span<const std::byte>(view.value().boot_sector_bytes)));

    ports::BlockDeviceGeometry target_geometry = request.target_geometry;
    if (target_geometry.capacity_bytes == 0) {
        target_geometry.capacity_bytes = request.target_capacity_bytes;
    }
    if (target_geometry.logical_sector_size == 0) {
        target_geometry.logical_sector_size = view.value().geometry.bytes_per_sector;
    }
    if (target_geometry.physical_sector_size == 0) {
        target_geometry.physical_sector_size = target_geometry.logical_sector_size;
    }

    ShrinkPlanBuilder builder;
    builder.set_source_chain_fingerprint(request.source_chain_fingerprint)
        .set_source_volume_index(request.source_volume_index)
        .set_source_logical_size_bytes(request.expected_source_logical_size_bytes)
        .set_source_boot_digest(boot_digest)
        .set_source_ntfs_geometry(view.value().geometry)
        .set_target_stable_id_digest(request.target_stable_id_digest)
        .set_target_device_geometry(target_geometry)
        .set_target_capacity_bytes(request.target_capacity_bytes)
        .set_new_total_sector_count(new_total_sector_count)
        .set_new_total_cluster_count(new_total_cluster_count)
        .set_new_mft_start_lcn(new_mft_start_lcn)
        .set_new_mft_mirror_start_lcn(new_mft_mirror_start_lcn)
        .set_minimum_target_bytes(minimum_target_bytes.value())
        .set_scratch_upper_bound_bytes(scratch_upper_bound.value())
        .set_protected_ranges(
            make_protected_ranges(view.value().geometry, request.target_capacity_bytes))
        .set_relocation_records(std::move(relocation.value().relocations))
        .set_metadata_mutations(std::move(relocation.value().mutations))
        .set_critical_file_operations(std::move(relocation.value().critical_operations));
    return builder.build();
}

} // namespace aegra::ntfs_resize
