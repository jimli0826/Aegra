#include "shrink_plan_internal.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/runlist.h"

#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] bool ranges_overlap_half_open(const std::uint64_t a0, const std::uint64_t a1,
                                            const std::uint64_t b0, const std::uint64_t b1) noexcept {
    return a0 < b1 && b0 < a1;
}

[[nodiscard]] base::Result<void> require_half_open(const std::uint64_t begin,
                                                   const std::uint64_t end) {
    if (begin > end) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_range_invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_capacity_invariants(const ShrinkPlan& plan) {
    const auto& boot = plan.source_ntfs_geometry();
    std::uint64_t expected_cluster_bytes = 0;
    std::uint64_t required_source_bytes = 0;
    std::uint64_t minimum_target_bytes = 0;
    if (boot.bytes_per_sector == 0 || boot.sectors_per_cluster == 0 ||
        !ntfs_core::checked_mul_u64(boot.bytes_per_sector, boot.sectors_per_cluster,
                                    expected_cluster_bytes) ||
        expected_cluster_bytes != boot.bytes_per_cluster ||
        !ntfs_core::checked_add_u64(boot.volume_size_bytes.value, boot.bytes_per_sector,
                                    required_source_bytes) ||
        required_source_bytes != plan.source_logical_size_bytes()) {
        return base::Result<void>::failure(make_plan_error(
            base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_capacity_mismatch"));
    }
    if (plan.target_capacity_bytes() <= boot.bytes_per_sector ||
        plan.target_capacity_bytes() % boot.bytes_per_sector != 0) {
        return base::Result<void>::failure(make_plan_error(
            base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_capacity_mismatch"));
    }
    const auto target_sector_count = plan.target_capacity_bytes() / boot.bytes_per_sector;
    if (plan.new_total_sector_count() != target_sector_count - 1U ||
        plan.new_total_cluster_count() !=
            plan.new_total_sector_count() / boot.sectors_per_cluster ||
        !ntfs_core::checked_mul_u64(plan.new_total_cluster_count(), boot.bytes_per_cluster,
                                    minimum_target_bytes) ||
        !ntfs_core::checked_add_u64(minimum_target_bytes, boot.bytes_per_sector,
                                    minimum_target_bytes) ||
        plan.minimum_target_bytes() != minimum_target_bytes) {
        return base::Result<void>::failure(make_plan_error(
            base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_capacity_mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_metadata_kind(const MetadataMutationKind kind) {
    switch (kind) {
    case MetadataMutationKind::kRunlistReplace:
    case MetadataMutationKind::kAttributeResize:
    case MetadataMutationKind::kRecordRewrite:
    case MetadataMutationKind::kAttributeListUpdate:
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(
        make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_unknown_mutation_kind"));
}

[[nodiscard]] base::Result<void> validate_critical_kind(const CriticalFileOperationKind kind) {
    switch (kind) {
    case CriticalFileOperationKind::kRelocateData:
    case CriticalFileOperationKind::kInvalidateLogfile:
    case CriticalFileOperationKind::kUpdateVolumeSize:
    case CriticalFileOperationKind::kUpdateBitmap:
    case CriticalFileOperationKind::kRelocateMft:
    case CriticalFileOperationKind::kUpdateBootGeometry:
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(
        make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_unknown_operation_kind"));
}

void append_boot_geometry(std::vector<std::byte>& out, const ntfs_core::BootGeometry& geometry) {
    append_u32(out, geometry.bytes_per_sector);
    append_u32(out, geometry.sectors_per_cluster);
    append_u32(out, geometry.bytes_per_cluster);
    append_u32(out, geometry.bytes_per_mft_record);
    append_u32(out, geometry.bytes_per_index_record);
    append_u64(out, geometry.total_clusters);
    append_u64(out, geometry.volume_size_bytes.value);
    append_u64(out, geometry.mft_start_lcn.value);
    append_u64(out, geometry.mft_mirror_start_lcn.value);
    append_u64(out, geometry.volume_serial);
}

void append_device_geometry(std::vector<std::byte>& out,
                            const ports::BlockDeviceGeometry& geometry) {
    append_u32(out, geometry.logical_sector_size);
    append_u32(out, geometry.physical_sector_size);
    append_u64(out, geometry.capacity_bytes);
}

[[nodiscard]] base::Result<ntfs_core::BootGeometry>
read_boot_geometry(const std::span<const std::byte> data, std::size_t& offset) {
    ntfs_core::BootGeometry geometry{};
    const auto bps = read_u32(data, offset);
    const auto spc = read_u32(data, offset);
    const auto bpc = read_u32(data, offset);
    const auto bpm = read_u32(data, offset);
    const auto bpi = read_u32(data, offset);
    const auto total = read_u64(data, offset);
    const auto vol = read_u64(data, offset);
    const auto mft = read_u64(data, offset);
    const auto mft_mirror = read_u64(data, offset);
    const auto serial = read_u64(data, offset);
    if (!bps || !spc || !bpc || !bpm || !bpi || !total || !vol || !mft || !mft_mirror ||
        !serial) {
        return base::Result<ntfs_core::BootGeometry>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    geometry.bytes_per_sector = bps.value();
    geometry.sectors_per_cluster = spc.value();
    geometry.bytes_per_cluster = bpc.value();
    geometry.bytes_per_mft_record = bpm.value();
    geometry.bytes_per_index_record = bpi.value();
    geometry.total_clusters = total.value();
    geometry.volume_size_bytes.value = vol.value();
    geometry.mft_start_lcn.value = mft.value();
    geometry.mft_mirror_start_lcn.value = mft_mirror.value();
    geometry.volume_serial = serial.value();
    return base::Result<ntfs_core::BootGeometry>::success(geometry);
}

[[nodiscard]] base::Result<ports::BlockDeviceGeometry>
read_device_geometry(const std::span<const std::byte> data, std::size_t& offset) {
    const auto logical = read_u32(data, offset);
    const auto physical = read_u32(data, offset);
    const auto capacity = read_u64(data, offset);
    if (!logical || !physical || !capacity) {
        return base::Result<ports::BlockDeviceGeometry>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    ports::BlockDeviceGeometry geometry{};
    geometry.logical_sector_size = logical.value();
    geometry.physical_sector_size = physical.value();
    geometry.capacity_bytes = capacity.value();
    return base::Result<ports::BlockDeviceGeometry>::success(geometry);
}

void append_section(std::vector<std::byte>& out, const std::uint32_t section_id,
                    const std::vector<std::byte>& body) {
    append_u32(out, section_id);
    append_u32(out, static_cast<std::uint32_t>(body.size()));
    append_bytes(out, body);
}

[[nodiscard]] base::Result<std::vector<std::byte>> encode_scalars_section(const ShrinkPlan& plan) {
    std::vector<std::byte> body;
    append_u32(body, plan.plan_version());
    append_utf8(body, plan.source_chain_fingerprint());
    append_u32(body, plan.source_volume_index());
    append_u64(body, plan.source_logical_size_bytes());
    append_utf8(body, plan.source_boot_digest());
    append_boot_geometry(body, plan.source_ntfs_geometry());
    append_utf8(body, plan.target_stable_id_digest());
    append_device_geometry(body, plan.target_device_geometry());
    append_u64(body, plan.target_capacity_bytes());
    append_u64(body, plan.new_total_sector_count());
    append_u64(body, plan.new_total_cluster_count());
    append_u64(body, plan.new_mft_start_lcn());
    append_u64(body, plan.new_mft_mirror_start_lcn());
    append_u64(body, plan.minimum_target_bytes());
    append_u64(body, plan.scratch_upper_bound_bytes());
    return base::Result<std::vector<std::byte>>::success(std::move(body));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_protected_section(const ShrinkPlan& plan) {
    if (plan.protected_ranges().size() > kMaxRecordCount) {
        return base::Result<std::vector<std::byte>>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<std::byte> body;
    append_u32(body, static_cast<std::uint32_t>(plan.protected_ranges().size()));
    for (const ByteRange& range : plan.protected_ranges()) {
        append_u64(body, range.begin);
        append_u64(body, range.end);
    }
    return base::Result<std::vector<std::byte>>::success(std::move(body));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_relocation_section(const ShrinkPlan& plan) {
    if (plan.relocation_records().size() > kMaxRecordCount) {
        return base::Result<std::vector<std::byte>>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<std::byte> body;
    append_u32(body, static_cast<std::uint32_t>(plan.relocation_records().size()));
    for (const RelocationRecord& record : plan.relocation_records()) {
        append_u64(body, record.source.begin_lcn);
        append_u64(body, record.source.end_lcn);
        append_u64(body, record.target.begin_lcn);
        append_u64(body, record.target.end_lcn);
        append_u64(body, record.cluster_count);
        append_u64(body, record.mft_record_number);
        append_u32(body, record.attribute_type);
        append_utf16(body, record.attribute_name);
        append_u16(body, record.attribute_id);
        append_u16(body, 0);
        append_u32(body, record.plan_order);
    }
    return base::Result<std::vector<std::byte>>::success(std::move(body));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_mutation_section(const ShrinkPlan& plan) {
    if (plan.metadata_mutations().size() > kMaxRecordCount) {
        return base::Result<std::vector<std::byte>>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<std::byte> body;
    append_u32(body, static_cast<std::uint32_t>(plan.metadata_mutations().size()));
    for (const MetadataMutation& mutation : plan.metadata_mutations()) {
        append_u64(body, mutation.mft_record_number);
        append_u16(body, mutation.expected_record_sequence);
        append_u16(body, 0);
        append_u32(body, mutation.attribute_type);
        append_utf16(body, mutation.attribute_name);
        append_u16(body, mutation.attribute_id);
        append_u16(body, 0);
        append_u32(body, mutation.attribute_record_offset);
        append_u32(body, mutation.attribute_length);
        append_u16(body, mutation.runlist_offset);
        append_u16(body, 0);
        append_u32(body, static_cast<std::uint32_t>(mutation.kind));
        append_utf8(body, mutation.preimage_digest);
        append_u32(body, static_cast<std::uint32_t>(mutation.replacement_runlist.size()));
        append_bytes(body, mutation.replacement_runlist);
    }
    return base::Result<std::vector<std::byte>>::success(std::move(body));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_critical_section(const ShrinkPlan& plan) {
    if (plan.critical_file_operations().size() > kMaxRecordCount) {
        return base::Result<std::vector<std::byte>>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<std::byte> body;
    append_u32(body, static_cast<std::uint32_t>(plan.critical_file_operations().size()));
    for (const CriticalFileOperation& op : plan.critical_file_operations()) {
        append_u32(body, op.file_number);
        append_u32(body, static_cast<std::uint32_t>(op.operation_kind));
        append_u32(body, op.order);
    }
    return base::Result<std::vector<std::byte>>::success(std::move(body));
}

[[nodiscard]] base::Result<void> parse_scalars(const std::span<const std::byte> body,
                                               ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto version = read_u32(body, offset);
    const auto fingerprint = read_utf8(body, offset);
    const auto volume_index = read_u32(body, offset);
    const auto logical_size = read_u64(body, offset);
    const auto boot_digest = read_utf8(body, offset);
    const auto boot = read_boot_geometry(body, offset);
    const auto target_digest = read_utf8(body, offset);
    const auto device = read_device_geometry(body, offset);
    const auto target_capacity = read_u64(body, offset);
    const auto sectors = read_u64(body, offset);
    const auto clusters = read_u64(body, offset);
    const auto mft = read_u64(body, offset);
    const auto mft_mirror = read_u64(body, offset);
    const auto minimum = read_u64(body, offset);
    const auto scratch = read_u64(body, offset);
    if (!version || !fingerprint || !volume_index || !logical_size || !boot_digest || !boot ||
        !target_digest || !device || !target_capacity || !sectors || !clusters || !mft ||
        !mft_mirror || !minimum || !scratch) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    if (offset != body.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    builder.set_plan_version(version.value())
        .set_source_chain_fingerprint(fingerprint.value())
        .set_source_volume_index(volume_index.value())
        .set_source_logical_size_bytes(logical_size.value())
        .set_source_boot_digest(boot_digest.value())
        .set_source_ntfs_geometry(boot.value())
        .set_target_stable_id_digest(target_digest.value())
        .set_target_device_geometry(device.value())
        .set_target_capacity_bytes(target_capacity.value())
        .set_new_total_sector_count(sectors.value())
        .set_new_total_cluster_count(clusters.value())
        .set_new_mft_start_lcn(mft.value())
        .set_new_mft_mirror_start_lcn(mft_mirror.value())
        .set_minimum_target_bytes(minimum.value())
        .set_scratch_upper_bound_bytes(scratch.value());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> parse_protected(const std::span<const std::byte> body,
                                                 ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto count = read_u32(body, offset);
    if (!count) {
        return base::Result<void>::failure(count.error());
    }
    if (count.value() > kMaxRecordCount) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<ByteRange> ranges;
    ranges.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
        const auto begin = read_u64(body, offset);
        const auto end = read_u64(body, offset);
        if (!begin || !end) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        ranges.push_back(ByteRange{.begin = begin.value(), .end = end.value()});
    }
    if (offset != body.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    builder.set_protected_ranges(std::move(ranges));
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> parse_relocations(const std::span<const std::byte> body,
                                                   ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto count = read_u32(body, offset);
    if (!count) {
        return base::Result<void>::failure(count.error());
    }
    if (count.value() > kMaxRecordCount) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<RelocationRecord> records;
    records.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
        RelocationRecord record{};
        const auto s0 = read_u64(body, offset);
        const auto s1 = read_u64(body, offset);
        const auto t0 = read_u64(body, offset);
        const auto t1 = read_u64(body, offset);
        const auto clusters = read_u64(body, offset);
        const auto mft = read_u64(body, offset);
        const auto attr_type = read_u32(body, offset);
        const auto name = read_utf16(body, offset);
        const auto attr_id = read_u16(body, offset);
        const auto reserved = read_u16(body, offset);
        const auto order = read_u32(body, offset);
        if (!s0 || !s1 || !t0 || !t1 || !clusters || !mft || !attr_type || !name || !attr_id ||
            !reserved || !order) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        if (reserved.value() != 0) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_reserved_nonzero"));
        }
        record.source = ClusterRange{.begin_lcn = s0.value(), .end_lcn = s1.value()};
        record.target = ClusterRange{.begin_lcn = t0.value(), .end_lcn = t1.value()};
        record.cluster_count = clusters.value();
        record.mft_record_number = mft.value();
        record.attribute_type = attr_type.value();
        record.attribute_name = name.value();
        record.attribute_id = attr_id.value();
        record.plan_order = order.value();
        records.push_back(std::move(record));
    }
    if (offset != body.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    builder.set_relocation_records(std::move(records));
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> parse_mutations(const std::span<const std::byte> body,
                                                 ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto count = read_u32(body, offset);
    if (!count) {
        return base::Result<void>::failure(count.error());
    }
    if (count.value() > kMaxRecordCount) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<MetadataMutation> mutations;
    mutations.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
        MetadataMutation mutation{};
        const auto mft = read_u64(body, offset);
        const auto sequence = read_u16(body, offset);
        const auto sequence_reserved = read_u16(body, offset);
        const auto attr_type = read_u32(body, offset);
        const auto name = read_utf16(body, offset);
        const auto attr_id = read_u16(body, offset);
        const auto reserved = read_u16(body, offset);
        const auto attr_offset = read_u32(body, offset);
        const auto attr_length = read_u32(body, offset);
        const auto runlist_offset = read_u16(body, offset);
        const auto runlist_reserved = read_u16(body, offset);
        const auto kind = read_u32(body, offset);
        const auto preimage = read_utf8(body, offset);
        const auto replacement_size = read_u32(body, offset);
        if (!mft || !sequence || !sequence_reserved || !attr_type || !name || !attr_id ||
            !reserved || !attr_offset || !attr_length || !runlist_offset || !runlist_reserved ||
            !kind || !preimage || !replacement_size ||
            replacement_size.value() > ntfs_core::kMaxRunlistEncodedBytes ||
            offset > body.size() || replacement_size.value() > body.size() - offset) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        if (sequence_reserved.value() != 0 || reserved.value() != 0 ||
            runlist_reserved.value() != 0) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_reserved_nonzero"));
        }
        mutation.mft_record_number = mft.value();
        mutation.expected_record_sequence = sequence.value();
        mutation.attribute_type = attr_type.value();
        mutation.attribute_name = name.value();
        mutation.attribute_id = attr_id.value();
        mutation.attribute_record_offset = attr_offset.value();
        mutation.attribute_length = attr_length.value();
        mutation.runlist_offset = runlist_offset.value();
        mutation.kind = static_cast<MetadataMutationKind>(kind.value());
        mutation.preimage_digest = preimage.value();
        mutation.replacement_runlist.assign(body.begin() + static_cast<std::ptrdiff_t>(offset),
                                            body.begin() + static_cast<std::ptrdiff_t>(
                                                               offset + replacement_size.value()));
        offset += replacement_size.value();
        mutations.push_back(std::move(mutation));
    }
    if (offset != body.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    builder.set_metadata_mutations(std::move(mutations));
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> parse_critical(const std::span<const std::byte> body,
                                                ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto count = read_u32(body, offset);
    if (!count) {
        return base::Result<void>::failure(count.error());
    }
    if (count.value() > kMaxRecordCount) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_too_many_records"));
    }
    std::vector<CriticalFileOperation> operations;
    operations.reserve(count.value());
    for (std::uint32_t i = 0; i < count.value(); ++i) {
        const auto file_number = read_u32(body, offset);
        const auto kind = read_u32(body, offset);
        const auto order = read_u32(body, offset);
        if (!file_number || !kind || !order) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        operations.push_back(CriticalFileOperation{
            .file_number = file_number.value(),
            .operation_kind = static_cast<CriticalFileOperationKind>(kind.value()),
            .order = order.value()});
    }
    if (offset != body.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    builder.set_critical_file_operations(std::move(operations));
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_plan_invariants(const ShrinkPlan& plan) {
    if (plan.plan_version() != kShrinkPlanVersion) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kUnsupportedVersion, "ntfs_resize.plan_unknown_version"));
    }
    if (!is_lowercase_hex64(plan.source_boot_digest()) ||
        !is_lowercase_hex64(plan.target_stable_id_digest())) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_digest_invalid"));
    }
    if (plan.target_capacity_bytes() != plan.target_device_geometry().capacity_bytes) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_capacity_mismatch"));
    }
    if (auto capacity = validate_capacity_invariants(plan); !capacity) {
        return capacity;
    }
    if (plan.minimum_target_bytes() > plan.target_capacity_bytes()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_minimum_too_large"));
    }
    if (plan.new_mft_start_lcn() >= plan.new_total_cluster_count() ||
        plan.new_mft_mirror_start_lcn() >= plan.new_total_cluster_count()) {
        return base::Result<void>::failure(make_plan_error(
            base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_boot_lcn_out_of_range"));
    }

    for (const ByteRange& range : plan.protected_ranges()) {
        auto status = require_half_open(range.begin, range.end);
        if (!status) {
            return status;
        }
    }

    std::vector<ClusterRange> targets;
    targets.reserve(plan.relocation_records().size());
    for (const RelocationRecord& record : plan.relocation_records()) {
        auto source_ok = require_half_open(record.source.begin_lcn, record.source.end_lcn);
        if (!source_ok) {
            return source_ok;
        }
        auto target_ok = require_half_open(record.target.begin_lcn, record.target.end_lcn);
        if (!target_ok) {
            return target_ok;
        }
        const std::uint64_t source_count = record.source.end_lcn - record.source.begin_lcn;
        const std::uint64_t target_count = record.target.end_lcn - record.target.begin_lcn;
        if (record.cluster_count != source_count || record.cluster_count != target_count) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_count_mismatch"));
        }
        for (const ClusterRange& prior : targets) {
            if (ranges_overlap_half_open(prior.begin_lcn, prior.end_lcn, record.target.begin_lcn,
                                         record.target.end_lcn)) {
                return base::Result<void>::failure(
                    make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_target_overlap"));
            }
        }
        targets.push_back(record.target);
    }

    for (const MetadataMutation& mutation : plan.metadata_mutations()) {
        auto status = validate_metadata_kind(mutation.kind);
        if (!status) {
            return status;
        }
        if (mutation.attribute_length == 0 ||
            mutation.runlist_offset >= mutation.attribute_length ||
            mutation.replacement_runlist.empty() ||
            mutation.replacement_runlist.size() >
                mutation.attribute_length - mutation.runlist_offset ||
            mutation.replacement_runlist.back() != std::byte{0} ||
            !is_lowercase_hex64(mutation.preimage_digest)) {
            return base::Result<void>::failure(make_plan_error(
                base::ErrorCode::kInvalidArgument, "ntfs_resize.plan_mutation_invalid"));
        }
    }
    for (const CriticalFileOperation& operation : plan.critical_file_operations()) {
        auto status = validate_critical_kind(operation.operation_kind);
        if (!status) {
            return status;
        }
    }
    return base::Result<void>::success();
}

base::Result<std::vector<std::byte>> encode_canonical_payload(const ShrinkPlan& plan) {
    const auto scalars = encode_scalars_section(plan);
    const auto protected_ranges = encode_protected_section(plan);
    const auto relocations = encode_relocation_section(plan);
    const auto mutations = encode_mutation_section(plan);
    const auto critical = encode_critical_section(plan);
    if (!scalars || !protected_ranges || !relocations || !mutations || !critical) {
        const base::Error& error = !scalars           ? scalars.error()
                                   : !protected_ranges ? protected_ranges.error()
                                   : !relocations      ? relocations.error()
                                   : !mutations        ? mutations.error()
                                                       : critical.error();
        return base::Result<std::vector<std::byte>>::failure(error);
    }

    std::vector<std::byte> payload;
    append_u32(payload, kSectionCount);
    append_section(payload, kSectionScalars, scalars.value());
    append_section(payload, kSectionProtectedRanges, protected_ranges.value());
    append_section(payload, kSectionRelocations, relocations.value());
    append_section(payload, kSectionMutations, mutations.value());
    append_section(payload, kSectionCriticalOps, critical.value());
    return base::Result<std::vector<std::byte>>::success(std::move(payload));
}

base::Result<void> parse_canonical_payload(const std::span<const std::byte> payload,
                                           ShrinkPlanBuilder& builder) {
    std::size_t offset = 0;
    const auto section_count = read_u32(payload, offset);
    if (!section_count) {
        return base::Result<void>::failure(section_count.error());
    }
    if (section_count.value() != kSectionCount) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_section_count"));
    }

    bool seen[6] = {};
    for (std::uint32_t i = 0; i < section_count.value(); ++i) {
        const auto section_id = read_u32(payload, offset);
        const auto section_size = read_u32(payload, offset);
        if (!section_id || !section_size) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        if (section_id.value() == 0 || section_id.value() > kSectionCount) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_unknown_section"));
        }
        if (seen[section_id.value()]) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_duplicate_section"));
        }
        seen[section_id.value()] = true;
        if (offset > payload.size() || section_size.value() > payload.size() - offset) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
        }
        const auto body = payload.subspan(offset, section_size.value());
        offset += section_size.value();

        base::Result<void> status = base::Result<void>::success();
        switch (section_id.value()) {
        case kSectionScalars:
            status = parse_scalars(body, builder);
            break;
        case kSectionProtectedRanges:
            status = parse_protected(body, builder);
            break;
        case kSectionRelocations:
            status = parse_relocations(body, builder);
            break;
        case kSectionMutations:
            status = parse_mutations(body, builder);
            break;
        case kSectionCriticalOps:
            status = parse_critical(body, builder);
            break;
        default:
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_unknown_section"));
        }
        if (!status) {
            return status;
        }
    }
    if (offset != payload.size()) {
        return base::Result<void>::failure(
            make_plan_error(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt"));
    }
    for (std::uint32_t id = 1; id <= kSectionCount; ++id) {
        if (!seen[id]) {
            return base::Result<void>::failure(
                make_plan_error(base::ErrorCode::kCorruptData, "ntfs_resize.plan_missing_section"));
        }
    }
    return base::Result<void>::success();
}

} // namespace aegra::ntfs_resize::detail
