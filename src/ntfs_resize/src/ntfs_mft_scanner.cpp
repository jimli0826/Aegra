#include "ntfs_mft_scanner.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/attribute_list.h"
#include "aegra/ntfs_core/bitmap.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/fixup.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/runlist.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <tuple>
#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] bool extent_less(const OutboundExtent& left, const OutboundExtent& right) noexcept {
    if (left.mft_record_number != right.mft_record_number) {
        return left.mft_record_number < right.mft_record_number;
    }
    if (left.attribute_type != right.attribute_type) {
        return left.attribute_type < right.attribute_type;
    }
    if (left.attribute_name != right.attribute_name) {
        return left.attribute_name < right.attribute_name;
    }
    if (left.attribute_id != right.attribute_id) {
        return left.attribute_id < right.attribute_id;
    }
    return left.run.first_lcn.value < right.run.first_lcn.value;
}

[[nodiscard]] base::Result<void>
append_outbound_from_attribute(const ntfs_core::ParsedMftRecord& record,
                               const ntfs_core::AttributeValue& attribute,
                               const std::uint64_t new_total_cluster_count, MftScanResult& result) {
    if (!attribute.non_resident || attribute.runs.empty()) {
        return base::Result<void>::success();
    }

    OwnedAttributeRuns owned;
    owned.mft_record_number = record.record_number;
    owned.record_sequence = record.sequence_number;
    owned.attribute_type = attribute.type;
    owned.attribute_name = attribute.name;
    owned.attribute_id = attribute.attribute_id;
    owned.compressed = attribute.compressed;
    owned.encrypted = attribute.encrypted;
    owned.attribute_record_offset = attribute.record_offset;
    owned.attribute_length = attribute.attribute_length;
    owned.runlist_offset = attribute.runlist_offset;
    if (attribute.runlist_offset >= attribute.attribute_length) {
        return shrink_fail_void(base::ErrorCode::kCorruptData,
                                "restore.shrink_unsupported_layout");
    }
    owned.runlist_capacity_bytes = attribute.attribute_length - attribute.runlist_offset;
    owned.runs = attribute.runs;

    bool has_outbound = false;
    for (const auto& run : attribute.runs) {
        if (run.sparse || run.cluster_count.value == 0) {
            continue;
        }
        std::uint64_t end_lcn = 0;
        if (!ntfs_core::checked_add_u64(run.first_lcn.value, run.cluster_count.value, end_lcn)) {
            return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
        }
        if (end_lcn <= new_total_cluster_count) {
            continue;
        }
        if (attribute.compressed || attribute.encrypted) {
            return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                    "restore.shrink_unsupported_layout");
        }

        OutboundExtent extent;
        extent.mft_record_number = record.record_number;
        extent.attribute_type = attribute.type;
        extent.attribute_name = attribute.name;
        extent.attribute_id = attribute.attribute_id;
        extent.attribute_compressed = attribute.compressed;
        extent.attribute_encrypted = attribute.encrypted;
        extent.run = run;
        if (run.first_lcn.value < new_total_cluster_count) {
            extent.run.first_lcn.value = new_total_cluster_count;
            extent.run.cluster_count.value = end_lcn - new_total_cluster_count;
            const auto skipped = new_total_cluster_count - run.first_lcn.value;
            extent.run.first_vcn.value = run.first_vcn.value + skipped;
        }
        result.allocated_beyond_clusters += extent.run.cluster_count.value;
        result.outbound_extents.push_back(std::move(extent));
        has_outbound = true;
    }
    if (has_outbound) {
        result.attributes_with_outbound.push_back(std::move(owned));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
scan_record_attributes(const ntfs_core::ParsedMftRecord& record,
                       const std::uint64_t new_total_cluster_count, MftScanResult& result) {
    for (const auto& attribute : record.attributes) {
        auto status =
            append_outbound_from_attribute(record, attribute, new_total_cluster_count, result);
        if (!status) {
            return status;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool attribute_list_target_exists(
    const ntfs_core::ParsedMftRecord& record,
    const ntfs_core::AttributeListEntry& entry) noexcept {
    for (const auto& attribute : record.attributes) {
        if (attribute.type != entry.type || attribute.name != entry.name ||
            attribute.attribute_id != entry.attribute_id) {
            continue;
        }
        if (!attribute.non_resident) {
            return entry.start_vcn.value == 0;
        }
        return !attribute.runs.empty() &&
               attribute.runs.front().first_vcn.value == entry.start_vcn.value;
    }
    return false;
}

[[nodiscard]] base::Result<void>
validate_attribute_lists(NtfsVolumeView& view, const ntfs_core::ParsedMftRecord& record,
                         const base::CancellationToken cancellation) {
    using EntryKey = std::tuple<std::uint32_t, std::u16string, std::uint64_t>;
    for (const auto& attribute : record.attributes) {
        if (attribute.type != ntfs_core::kAttrAttributeList) {
            continue;
        }
        if (attribute.non_resident || record.base_record != 0) {
            return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                    "restore.shrink_unsupported_layout");
        }
        auto entries = ntfs_core::parse_attribute_list(attribute.resident_data);
        if (!entries) {
            return base::Result<void>::failure(entries.error());
        }
        std::set<EntryKey> seen;
        for (const auto& entry : entries.value()) {
            if (!seen.emplace(entry.type, entry.name, entry.start_vcn.value).second) {
                return shrink_fail_void(base::ErrorCode::kCorruptData,
                                        "restore.shrink_unsupported_layout");
            }
            auto target = entry.attribute_record.record_number == record.record_number
                              ? base::Result<ntfs_core::ParsedMftRecord>::success(record)
                              : read_mft_record(view, entry.attribute_record.record_number,
                                                cancellation);
            if (!target || !target.value().in_use ||
                target.value().sequence_number != entry.attribute_record.sequence_number ||
                (target.value().record_number != record.record_number &&
                 target.value().base_record != record.record_number) ||
                !attribute_list_target_exists(target.value(), entry)) {
                return shrink_fail_void(base::ErrorCode::kCorruptData,
                                        "restore.shrink_unsupported_layout");
            }
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
check_volume_dirty_flag(NtfsVolumeView& view, const base::CancellationToken cancellation) {
    auto record = read_mft_record(view, kFileNumberVolume, cancellation);
    if (!record) {
        return base::Result<void>::failure(record.error());
    }
    for (const auto& attribute : record.value().attributes) {
        if (attribute.type != kAttrVolumeInformation || attribute.non_resident) {
            continue;
        }
        if (attribute.resident_data.size() < 12) {
            return shrink_fail_void(base::ErrorCode::kCorruptData,
                                    "restore.shrink_unsupported_layout");
        }
        const auto flags = ntfs_core::read_u16(attribute.resident_data, 0x0A);
        if ((flags & kVolumeFlagDirty) != 0) {
            return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                    "restore.shrink_unsupported_layout");
        }
        return base::Result<void>::success();
    }
    return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
}

[[nodiscard]] base::Result<void>
read_logfile_prefix(NtfsVolumeView& view, const ntfs_core::AttributeValue& logfile_data,
                    const std::size_t size, std::vector<std::byte>& output,
                    const base::CancellationToken cancellation) {
    output.resize(size);
    auto read = ntfs_core::read_from_attribute(
        *view.reader, view.geometry, logfile_data, ntfs_core::ByteOffset{0}, output, cancellation);
    if (!read || read.value() != output.size()) {
        return read ? shrink_fail_void(base::ErrorCode::kCorruptData,
                                       "restore.shrink_unsupported_layout")
                    : base::Result<void>::failure(read.error());
    }
    return base::Result<void>::success();
}

struct RestartPageEvidence final {
    std::uint64_t current_lsn{0};
    std::uint64_t logfile_size{0};
    std::uint32_t system_page_size{0};
    std::uint32_t log_page_size{0};
    std::uint16_t major_version{0};
    bool clean{false};
};

[[nodiscard]] base::Result<RestartPageEvidence>
parse_restart_page(std::vector<std::byte> page, const std::uint32_t bytes_per_sector) {
    constexpr std::size_t kRestartAreaMinimumBytes = 0x2CU;
    if (page.size() < 512 || std::memcmp(page.data(), "RSTR", 4) != 0) {
        return shrink_fail<RestartPageEvidence>(base::ErrorCode::kCorruptData,
                                                "restore.shrink_unsupported_layout");
    }
    const auto usa_offset = ntfs_core::read_u16(page, 4);
    const auto usa_count = ntfs_core::read_u16(page, 6);
    if (usa_count != page.size() / bytes_per_sector + 1U) {
        return shrink_fail<RestartPageEvidence>(base::ErrorCode::kCorruptData,
                                                "restore.shrink_unsupported_layout");
    }
    if (auto fixed = ntfs_core::apply_fixup(page, bytes_per_sector, usa_offset, usa_count); !fixed) {
        return base::Result<RestartPageEvidence>::failure(fixed.error());
    }
    RestartPageEvidence evidence;
    evidence.system_page_size = ntfs_core::read_u32(page, 0x10);
    evidence.log_page_size = ntfs_core::read_u32(page, 0x14);
    const auto restart_offset = ntfs_core::read_u16(page, 0x18);
    evidence.major_version = ntfs_core::read_u16(page, 0x1C);
    if (evidence.system_page_size != page.size() || evidence.log_page_size < 512 ||
        (evidence.log_page_size & (evidence.log_page_size - 1U)) != 0 ||
        restart_offset > page.size() || kRestartAreaMinimumBytes > page.size() - restart_offset ||
        evidence.major_version == 0) {
        return shrink_fail<RestartPageEvidence>(base::ErrorCode::kCorruptData,
                                                "restore.shrink_unsupported_layout");
    }
    evidence.current_lsn = ntfs_core::read_u64(page, restart_offset);
    evidence.clean = (ntfs_core::read_u16(page, restart_offset + 0x0E) & 0x0002U) != 0;
    evidence.logfile_size = ntfs_core::read_u64(page, restart_offset + 0x18);
    return base::Result<RestartPageEvidence>::success(evidence);
}

[[nodiscard]] base::Result<void>
check_logfile_restart_state(NtfsVolumeView& view, const base::CancellationToken cancellation) {
    auto logfile_data = load_unnamed_data_attribute(view, kFileNumberLogFile, cancellation);
    if (!logfile_data) {
        return base::Result<void>::failure(logfile_data.error());
    }
    if (logfile_data.value().data_size.value < 512) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    std::vector<std::byte> header;
    if (auto read = read_logfile_prefix(view, logfile_data.value(), 512, header, cancellation);
        !read) {
        return read;
    }
    const auto system_page_size = ntfs_core::read_u32(header, 0x10);
    if (system_page_size < 512 || system_page_size > 64U * 1024U ||
        (system_page_size & (system_page_size - 1U)) != 0 ||
        system_page_size % view.geometry.bytes_per_sector != 0 ||
        logfile_data.value().data_size.value < static_cast<std::uint64_t>(system_page_size) * 2U) {
        return shrink_fail_void(base::ErrorCode::kCorruptData,
                                "restore.shrink_unsupported_layout");
    }
    std::vector<std::byte> restart_pages;
    if (auto read = read_logfile_prefix(view, logfile_data.value(), system_page_size * 2U,
                                        restart_pages, cancellation);
        !read) {
        return read;
    }
    auto first = parse_restart_page(
        std::vector<std::byte>(restart_pages.begin(), restart_pages.begin() + system_page_size),
        view.geometry.bytes_per_sector);
    auto second = parse_restart_page(
        std::vector<std::byte>(restart_pages.begin() + system_page_size, restart_pages.end()),
        view.geometry.bytes_per_sector);
    if (!first || !second) {
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    const auto& newest = first.value().current_lsn >= second.value().current_lsn ? first.value()
                                                                                : second.value();
    if (first.value().system_page_size != second.value().system_page_size ||
        first.value().log_page_size != second.value().log_page_size ||
        first.value().major_version != second.value().major_version ||
        newest.logfile_size != logfile_data.value().data_size.value || !newest.clean) {
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<std::byte>>
load_mft_allocation_bitmap(NtfsVolumeView& view, const std::uint64_t record_count,
                           const base::CancellationToken cancellation) {
    auto attribute =
        load_unnamed_attribute(view, kFileNumberMft, ntfs_core::kAttrBitmap, cancellation);
    if (!attribute) {
        return base::Result<std::vector<std::byte>>::failure(attribute.error());
    }
    auto covers =
        ntfs_core::validate_bitmap_covers_bits(record_count, attribute.value().data_size.value);
    if (!covers) {
        return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                   "restore.shrink_unsupported_layout");
    }
    return read_attribute_payload(view, attribute.value(), kMaxBitmapLoadBytes, cancellation);
}

} // namespace

base::Result<void> reject_dirty_or_unsafe_logfile(NtfsVolumeView& view,
                                                  const base::CancellationToken cancellation) {
    auto dirty = check_volume_dirty_flag(view, cancellation);
    if (!dirty) {
        return dirty;
    }
    return check_logfile_restart_state(view, cancellation);
}

base::Result<MftScanResult>
scan_outbound_mft_extents(NtfsVolumeView& view, const std::uint64_t new_total_cluster_count,
                          const base::CancellationToken cancellation) {
    if (view.mft_data.data_size.value < view.geometry.bytes_per_mft_record ||
        view.mft_data.data_size.value % view.geometry.bytes_per_mft_record != 0) {
        return shrink_fail<MftScanResult>(base::ErrorCode::kCorruptData,
                                          "restore.shrink_unsupported_layout");
    }
    const auto record_count =
        view.mft_data.data_size.value /
        static_cast<std::uint64_t>(view.geometry.bytes_per_mft_record);
    MftScanResult result;
    result.mft_record_count = record_count;
    auto mft_bitmap = load_mft_allocation_bitmap(view, record_count, cancellation);
    if (!mft_bitmap) {
        return base::Result<MftScanResult>::failure(mft_bitmap.error());
    }

    for (std::uint64_t record_number = 0; record_number < record_count; ++record_number) {
        if (cancellation.stop_requested()) {
            return shrink_fail<MftScanResult>(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        auto record_in_use = ntfs_core::bitmap_bit_is_set(mft_bitmap.value(), record_number);
        if (!record_in_use) {
            return base::Result<MftScanResult>::failure(record_in_use.error());
        }
        if (!record_in_use.value()) {
            continue;
        }
        auto record = read_mft_record(view, record_number, cancellation);
        if (!record) {
            return base::Result<MftScanResult>::failure(record.error());
        }
        if (!record.value().in_use) {
            return shrink_fail<MftScanResult>(base::ErrorCode::kCorruptData,
                                              "restore.shrink_unsupported_layout");
        }
        if (auto lists = validate_attribute_lists(view, record.value(), cancellation); !lists) {
            return base::Result<MftScanResult>::failure(lists.error());
        }
        auto status = scan_record_attributes(record.value(), new_total_cluster_count, result);
        if (!status) {
            return base::Result<MftScanResult>::failure(status.error());
        }
    }

    std::sort(result.outbound_extents.begin(), result.outbound_extents.end(), extent_less);
    std::sort(result.attributes_with_outbound.begin(), result.attributes_with_outbound.end(),
              [](const OwnedAttributeRuns& left, const OwnedAttributeRuns& right) {
                  if (left.mft_record_number != right.mft_record_number) {
                      return left.mft_record_number < right.mft_record_number;
                  }
                  if (left.attribute_type != right.attribute_type) {
                      return left.attribute_type < right.attribute_type;
                  }
                  if (left.attribute_name != right.attribute_name) {
                      return left.attribute_name < right.attribute_name;
                  }
                  return left.attribute_id < right.attribute_id;
              });
    return base::Result<MftScanResult>::success(std::move(result));
}

} // namespace aegra::ntfs_resize::detail
