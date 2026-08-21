#include "ntfs_metadata_editor.h"

#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"
#include "shrink_plan_internal.h"

#include "aegra/ntfs_core/attribute.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/fixup.h"
#include "aegra/ntfs_core/mft_record.h"
#include "aegra/ntfs_core/runlist.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] base::Result<void>
replace_attribute_runlist(std::span<std::byte> record_bytes,
                          const MetadataMutation& mutation) {
    const auto used_size = ntfs_core::read_u32(record_bytes, 0x18);
    const auto offset = static_cast<std::size_t>(mutation.attribute_record_offset);
    const auto length = mutation.attribute_length;
    if (used_size > record_bytes.size() || offset > used_size || length > used_size - offset ||
        mutation.runlist_offset >= length) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    auto parsed = ntfs_core::parse_attribute(record_bytes.subspan(0, used_size), offset);
    if (!parsed || !parsed.value().non_resident || parsed.value().type != mutation.attribute_type ||
        parsed.value().name != mutation.attribute_name ||
        parsed.value().attribute_id != mutation.attribute_id ||
        parsed.value().attribute_length != mutation.attribute_length ||
        parsed.value().runlist_offset != mutation.runlist_offset) {
        return shrink_fail_void(base::ErrorCode::kConflict, "restore.shrink_plan_changed");
    }
    const auto available = mutation.attribute_length - mutation.runlist_offset;
    auto current = ntfs_core::encode_runlist_bounded(parsed.value().runs, available);
    if (!current || digest_to_hex(sha256(current.value())) != mutation.preimage_digest ||
        mutation.replacement_runlist.size() > available) {
        return shrink_fail_void(base::ErrorCode::kConflict, "restore.shrink_plan_changed");
    }
    auto attr = record_bytes.subspan(offset, length);
    std::fill(attr.begin() + mutation.runlist_offset, attr.end(), std::byte{0});
    std::memcpy(attr.data() + mutation.runlist_offset, mutation.replacement_runlist.data(),
                mutation.replacement_runlist.size());
    return base::Result<void>::success();
}

} // namespace

base::Result<std::uint64_t>
apply_runlist_mutations(MftRecordStore& store, const ShrinkPlan& plan,
                        const std::uint64_t new_total_cluster_count, const RecordClassFilter filter,
                        const base::CancellationToken cancellation) {
    static_cast<void>(new_total_cluster_count);

    std::uint64_t updated = 0;
    for (const auto& mutation : plan.metadata_mutations()) {
        if (!matches_record_filter(filter, mutation.mft_record_number) ||
            mutation.kind != MetadataMutationKind::kRunlistReplace) {
            continue;
        }
        if (cancellation.stop_requested()) {
            return shrink_fail<std::uint64_t>(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        auto raw = store.read_record_bytes(mutation.mft_record_number, cancellation);
        if (!raw) {
            return base::Result<std::uint64_t>::failure(raw.error());
        }
        auto editable = std::move(raw).value();
        auto parsed = ntfs_core::parse_mft_record_bytes(
            editable, store.geometry().bytes_per_sector, mutation.mft_record_number);
        if (!parsed) {
            return base::Result<std::uint64_t>::failure(parsed.error());
        }
        if (parsed.value().sequence_number != mutation.expected_record_sequence) {
            return shrink_fail<std::uint64_t>(base::ErrorCode::kConflict,
                                              "restore.shrink_plan_changed");
        }
        auto replace = replace_attribute_runlist(editable, mutation);
        if (!replace) {
            return base::Result<std::uint64_t>::failure(replace.error());
        }
        auto updated_attribute = ntfs_core::parse_attribute(
            editable, static_cast<std::size_t>(mutation.attribute_record_offset));
        if (!updated_attribute) {
            return base::Result<std::uint64_t>::failure(updated_attribute.error());
        }
        const bool updates_mft_data = mutation.mft_record_number == 0 &&
                                      mutation.attribute_type == ntfs_core::kAttrData &&
                                      mutation.attribute_name.empty();
        if (updates_mft_data) {
            store.set_mft_data(updated_attribute.value());
        }
        auto written = store.write_record_bytes(mutation.mft_record_number, editable, cancellation);
        if (!written) {
            return base::Result<std::uint64_t>::failure(written.error());
        }
        ++updated;
    }
    return base::Result<std::uint64_t>::success(updated);
}

} // namespace aegra::ntfs_resize::detail
