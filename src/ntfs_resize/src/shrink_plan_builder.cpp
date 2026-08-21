#include "aegra/ntfs_resize/shrink_plan.h"

#include "shrink_plan_internal.h"

#include <utility>

namespace aegra::ntfs_resize {

ShrinkPlanBuilder& ShrinkPlanBuilder::set_plan_version(const std::uint32_t version) noexcept {
    draft_.plan_version_ = version;
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_source_chain_fingerprint(std::string fingerprint) {
    draft_.source_chain_fingerprint_ = std::move(fingerprint);
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_source_volume_index(const std::uint32_t index) noexcept {
    draft_.source_volume_index_ = index;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_source_logical_size_bytes(const std::uint64_t bytes) noexcept {
    draft_.source_logical_size_bytes_ = bytes;
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_source_boot_digest(std::string digest) {
    draft_.source_boot_digest_ = std::move(digest);
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_source_ntfs_geometry(ntfs_core::BootGeometry geometry) {
    draft_.source_ntfs_geometry_ = std::move(geometry);
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_target_stable_id_digest(std::string digest) {
    draft_.target_stable_id_digest_ = std::move(digest);
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_target_device_geometry(ports::BlockDeviceGeometry geometry) {
    draft_.target_device_geometry_ = geometry;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_target_capacity_bytes(const std::uint64_t bytes) noexcept {
    draft_.target_capacity_bytes_ = bytes;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_new_total_sector_count(const std::uint64_t count) noexcept {
    draft_.new_total_sector_count_ = count;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_new_total_cluster_count(const std::uint64_t count) noexcept {
    draft_.new_total_cluster_count_ = count;
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_new_mft_start_lcn(const std::uint64_t lcn) noexcept {
    draft_.new_mft_start_lcn_ = lcn;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_new_mft_mirror_start_lcn(const std::uint64_t lcn) noexcept {
    draft_.new_mft_mirror_start_lcn_ = lcn;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_minimum_target_bytes(const std::uint64_t bytes) noexcept {
    draft_.minimum_target_bytes_ = bytes;
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_scratch_upper_bound_bytes(const std::uint64_t bytes) noexcept {
    draft_.scratch_upper_bound_bytes_ = bytes;
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_protected_ranges(std::vector<ByteRange> ranges) {
    draft_.protected_ranges_ = std::move(ranges);
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_relocation_records(std::vector<RelocationRecord> records) {
    draft_.relocation_records_ = std::move(records);
    return *this;
}

ShrinkPlanBuilder&
ShrinkPlanBuilder::set_metadata_mutations(std::vector<MetadataMutation> mutations) {
    draft_.metadata_mutations_ = std::move(mutations);
    return *this;
}

ShrinkPlanBuilder& ShrinkPlanBuilder::set_critical_file_operations(
    std::vector<CriticalFileOperation> operations) {
    draft_.critical_file_operations_ = std::move(operations);
    return *this;
}

base::Result<ShrinkPlan> ShrinkPlanBuilder::build() {
    auto status = detail::validate_plan_invariants(draft_);
    if (!status) {
        return base::Result<ShrinkPlan>::failure(status.error());
    }

    auto payload = detail::encode_canonical_payload(draft_);
    if (!payload) {
        return base::Result<ShrinkPlan>::failure(payload.error());
    }

    const auto digest = detail::sha256(payload.value());
    draft_.plan_payload_digest_ = detail::digest_to_hex(digest);
    return base::Result<ShrinkPlan>::success(draft_);
}

} // namespace aegra::ntfs_resize
