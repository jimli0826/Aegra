#include "ntfs_volume_view.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/attribute_list.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/boot_sector.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"
#include "aegra/ntfs_core/runlist.h"

#include <algorithm>
#include <optional>
#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
find_unnamed_data(const ntfs_core::ParsedMftRecord& record) {
    for (const auto& attribute : record.attributes) {
        if (attribute.type == ntfs_core::kAttrData && attribute.name.empty()) {
            return base::Result<ntfs_core::AttributeValue>::success(attribute);
        }
    }
    return shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                   "restore.shrink_unsupported_layout");
}

[[nodiscard]] base::Result<void> merge_data_runs(ntfs_core::AttributeValue& destination,
                                                 const ntfs_core::AttributeValue& piece) {
    if (!piece.non_resident) {
        return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                "restore.shrink_unsupported_layout");
    }
    destination.non_resident = true;
    destination.compressed = destination.compressed || piece.compressed;
    destination.encrypted = destination.encrypted || piece.encrypted;
    destination.sparse = destination.sparse || piece.sparse;
    if (piece.data_size.value > destination.data_size.value) {
        destination.data_size = piece.data_size;
    }
    if (piece.allocated_size.value > destination.allocated_size.value) {
        destination.allocated_size = piece.allocated_size;
    }
    if (piece.initialized_size.value > destination.initialized_size.value) {
        destination.initialized_size = piece.initialized_size;
    }
    destination.runs.insert(destination.runs.end(), piece.runs.begin(), piece.runs.end());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> sort_and_validate_merged_runs(ntfs_core::AttributeValue& data) {
    std::sort(data.runs.begin(), data.runs.end(),
              [](const ntfs_core::DataRun& left, const ntfs_core::DataRun& right) {
                  return left.first_vcn.value < right.first_vcn.value;
              });
    return ntfs_core::validate_data_runs(data.runs);
}

[[nodiscard]] base::Result<std::uint64_t>
mft_record_file_offset(const NtfsVolumeView& view, const std::uint64_t record_number) {
    std::uint64_t offset = 0;
    if (!ntfs_core::checked_mul_u64(record_number, view.geometry.bytes_per_mft_record, offset)) {
        return shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                          "restore.shrink_unsupported_layout");
    }
    return base::Result<std::uint64_t>::success(offset);
}

[[nodiscard]] std::optional<std::uint64_t>
mapped_source_offset(const NtfsVolumeView& view, const std::uint64_t file_offset) noexcept {
    const auto file_vcn = file_offset / view.geometry.bytes_per_cluster;
    const auto cluster_offset = file_offset % view.geometry.bytes_per_cluster;
    for (const auto& run : view.mft_data.runs) {
        if (run.sparse || file_vcn < run.first_vcn.value ||
            file_vcn - run.first_vcn.value >= run.cluster_count.value) {
            continue;
        }
        std::uint64_t source_lcn = 0;
        std::uint64_t source_offset = 0;
        if (ntfs_core::checked_add_u64(run.first_lcn.value,
                                       file_vcn - run.first_vcn.value, source_lcn) &&
            ntfs_core::checked_mul_u64(source_lcn, view.geometry.bytes_per_cluster,
                                       source_offset) &&
            ntfs_core::checked_add_u64(source_offset, cluster_offset, source_offset)) {
            return source_offset;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_mft_record_bytes(NtfsVolumeView& view, const std::uint64_t file_offset,
                      std::optional<std::uint64_t>& source_device_offset,
                      const base::CancellationToken cancellation) {
    std::vector<std::byte> record(view.geometry.bytes_per_mft_record);
    if (!view.mft_ready) {
        std::uint64_t offset = 0;
        if (!ntfs_core::checked_mul_u64(view.geometry.mft_start_lcn.value,
                                        view.geometry.bytes_per_cluster, offset) ||
            !ntfs_core::checked_add_u64(offset, file_offset, offset)) {
            return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                        "restore.shrink_unsupported_layout");
        }
        source_device_offset = offset;
        return read_exact_bytes(*view.reader, offset, record.size(), cancellation);
    }
    source_device_offset = mapped_source_offset(view, file_offset);
    auto read = ntfs_core::read_from_attribute(
        *view.reader, view.geometry, view.mft_data, ntfs_core::ByteOffset{file_offset}, record,
        cancellation);
    if (!read) {
        return base::Result<std::vector<std::byte>>::failure(read.error());
    }
    if (read.value() != record.size()) {
        return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                    "restore.shrink_unsupported_layout");
    }
    return base::Result<std::vector<std::byte>>::success(std::move(record));
}

[[nodiscard]] NtfsShrinkMftRecordSnapshot
make_mft_record_snapshot(const NtfsVolumeView& view, const std::uint64_t record_number,
                         const std::uint64_t file_offset,
                         const std::optional<std::uint64_t> source_device_offset,
                         const std::span<const std::byte> record) noexcept {
    NtfsShrinkMftRecordSnapshot snapshot;
    snapshot.target_capacity_bytes = view.target_capacity_bytes;
    snapshot.record_number = record_number;
    snapshot.mft_file_offset_bytes = file_offset;
    snapshot.source_device_offset_known = source_device_offset.has_value();
    snapshot.source_device_offset_bytes = source_device_offset.value_or(0);
    snapshot.record_size_bytes = static_cast<std::uint32_t>(record.size());
    snapshot.read_via_mft_data = view.mft_ready;
    if (record.size() < 0x1CU) {
        return snapshot;
    }
    snapshot.signature_hex =
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(record[0])) << 24U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(record[1])) << 16U) |
        (static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(record[2])) << 8U) |
        static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(record[3]));
    snapshot.update_sequence_array_offset = ntfs_core::read_u16(record, 4);
    snapshot.update_sequence_array_count = ntfs_core::read_u16(record, 6);
    snapshot.first_attribute_offset = ntfs_core::read_u16(record, 0x14);
    snapshot.flags = ntfs_core::read_u16(record, 0x16);
    snapshot.used_size_bytes = ntfs_core::read_u32(record, 0x18);
    return snapshot;
}

[[nodiscard]] base::Result<ntfs_core::ParsedMftRecord>
read_mft_record_raw(NtfsVolumeView& view, const std::uint64_t record_number,
                    const base::CancellationToken cancellation) {
    auto file_offset = mft_record_file_offset(view, record_number);
    if (!file_offset) {
        return base::Result<ntfs_core::ParsedMftRecord>::failure(file_offset.error());
    }
    std::optional<std::uint64_t> source_device_offset;
    auto bytes = read_mft_record_bytes(view, file_offset.value(), source_device_offset,
                                       cancellation);
    if (!bytes) {
        return base::Result<ntfs_core::ParsedMftRecord>::failure(bytes.error());
    }
    auto snapshot = make_mft_record_snapshot(view, record_number, file_offset.value(),
                                             source_device_offset, bytes.value());
    auto parsed = ntfs_core::parse_mft_record_bytes(std::span<std::byte>(bytes.value()),
                                                    view.geometry.bytes_per_sector,
                                                    record_number);
    snapshot.parsed = parsed.has_value();
    if (!parsed) {
        snapshot.error_code = parsed.error().code;
        snapshot.message_code = parsed.error().message;
    }
    if (view.observer != nullptr && !parsed) {
        view.observer->mft_record(snapshot);
    }
    return parsed;
}

} // namespace

base::Result<std::vector<std::byte>>
read_exact_bytes(ports::IRandomAccessReader& reader, const std::uint64_t offset,
                 const std::size_t size, const base::CancellationToken cancellation) {
    std::vector<std::byte> buffer(size);
    std::size_t total = 0;
    while (total < size) {
        if (cancellation.stop_requested()) {
            return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCancelled,
                                                       "ntfs.read_failed");
        }
        auto n = reader.read_at(offset + total,
                                std::span<std::byte>(buffer.data() + total, size - total),
                                cancellation);
        if (!n) {
            return base::Result<std::vector<std::byte>>::failure(n.error());
        }
        if (n.value() == 0) {
            return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kIoFailure,
                                                       "ntfs.read_failed");
        }
        total += n.value();
    }
    return base::Result<std::vector<std::byte>>::success(std::move(buffer));
}

base::Result<NtfsVolumeView>
open_ntfs_volume_view(ports::IRandomAccessReader& reader,
                      const std::uint64_t expected_source_logical_size_bytes,
                      const base::CancellationToken cancellation,
                      const NtfsVolumeOpenDiagnostics& diagnostics) {
    auto boot_bytes = read_exact_bytes(reader, 0, 512, cancellation);
    if (!boot_bytes) {
        return base::Result<NtfsVolumeView>::failure(boot_bytes.error());
    }
    auto geometry = ntfs_core::parse_boot_sector(std::span<const std::byte>(boot_bytes.value()));
    if (!geometry) {
        return shrink_fail<NtfsVolumeView>(base::ErrorCode::kUnsupportedVersion,
                                           "restore.shrink_not_ntfs");
    }
    if (diagnostics.observer != nullptr) {
        diagnostics.observer->candidate_geometry(
            {expected_source_logical_size_bytes, diagnostics.target_capacity_bytes,
             geometry.value(), diagnostics.target_geometry});
    }
    std::uint64_t required_source_bytes = 0;
    if (!ntfs_core::checked_add_u64(geometry.value().volume_size_bytes.value,
                                    geometry.value().bytes_per_sector,
                                    required_source_bytes) ||
        required_source_bytes != expected_source_logical_size_bytes) {
        return shrink_fail<NtfsVolumeView>(base::ErrorCode::kInvalidArgument,
                                           "restore.shrink_sector_mismatch");
    }

    NtfsVolumeView view;
    view.reader = &reader;
    view.geometry = std::move(geometry).value();
    view.boot_sector_bytes = std::move(boot_bytes).value();
    view.observer = diagnostics.observer;
    view.target_capacity_bytes = diagnostics.target_capacity_bytes;

    auto mft0 = read_mft_record_raw(view, 0, cancellation);
    if (!mft0) {
        return base::Result<NtfsVolumeView>::failure(mft0.error());
    }
    auto base_data = find_unnamed_data(mft0.value());
    if (!base_data) {
        return base::Result<NtfsVolumeView>::failure(base_data.error());
    }
    view.mft_data = std::move(base_data).value();
    view.mft_ready = true;

    auto mft_data = load_unnamed_data_attribute(view, 0, cancellation);
    if (!mft_data) {
        return base::Result<NtfsVolumeView>::failure(mft_data.error());
    }
    view.mft_data = std::move(mft_data).value();
    return base::Result<NtfsVolumeView>::success(std::move(view));
}

base::Result<ntfs_core::ParsedMftRecord>
read_mft_record(NtfsVolumeView& view, const std::uint64_t record_number,
                const base::CancellationToken cancellation) {
    return read_mft_record_raw(view, record_number, cancellation);
}

base::Result<std::vector<std::uint64_t>>
collect_extension_record_numbers(NtfsVolumeView& view, const ntfs_core::ParsedMftRecord& base,
                                 const base::CancellationToken cancellation) {
    std::vector<std::uint64_t> extensions;
    for (const auto& attribute : base.attributes) {
        if (attribute.type != ntfs_core::kAttrAttributeList) {
            continue;
        }
        auto list_bytes = read_attribute_payload(view, attribute, ntfs_core::kMaxIndexRecordBytes,
                                                 cancellation);
        if (!list_bytes) {
            return base::Result<std::vector<std::uint64_t>>::failure(list_bytes.error());
        }
        auto parsed = ntfs_core::parse_attribute_list(std::span<const std::byte>(list_bytes.value()));
        if (!parsed) {
            return base::Result<std::vector<std::uint64_t>>::failure(parsed.error());
        }
        auto valid =
            ntfs_core::validate_attribute_list_entries(parsed.value(), base.record_number);
        if (!valid) {
            return base::Result<std::vector<std::uint64_t>>::failure(valid.error());
        }
        for (const auto& entry : parsed.value()) {
            const auto extension = entry.attribute_record.record_number;
            if (extension == base.record_number) {
                continue;
            }
            if (std::find(extensions.begin(), extensions.end(), extension) == extensions.end()) {
                extensions.push_back(extension);
            }
            if (extensions.size() > kMaxAttributeListExtensions) {
                return shrink_fail<std::vector<std::uint64_t>>(
                    base::ErrorCode::kUnsupportedVersion, "restore.shrink_unsupported_layout");
            }
        }
    }
    return base::Result<std::vector<std::uint64_t>>::success(std::move(extensions));
}

base::Result<ntfs_core::AttributeValue>
load_unnamed_attribute(NtfsVolumeView& view, const std::uint64_t record_number,
                       const std::uint32_t attribute_type,
                       const base::CancellationToken cancellation) {
    auto base = read_mft_record_raw(view, record_number, cancellation);
    if (!base) {
        return base::Result<ntfs_core::AttributeValue>::failure(base.error());
    }
    if (!base.value().in_use) {
        return shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                       "restore.shrink_unsupported_layout");
    }

    ntfs_core::AttributeValue merged{};
    merged.type = attribute_type;
    bool found = false;
    auto take = [&](const ntfs_core::ParsedMftRecord& record) -> base::Result<void> {
        for (const auto& attribute : record.attributes) {
            if (attribute.type != attribute_type || !attribute.name.empty()) {
                continue;
            }
            if (!found) {
                merged = attribute;
                found = true;
                continue;
            }
            auto status = merge_data_runs(merged, attribute);
            if (!status) {
                return status;
            }
        }
        return base::Result<void>::success();
    };

    auto base_status = take(base.value());
    if (!base_status) {
        return base::Result<ntfs_core::AttributeValue>::failure(base_status.error());
    }
    auto extensions = collect_extension_record_numbers(view, base.value(), cancellation);
    if (!extensions) {
        return base::Result<ntfs_core::AttributeValue>::failure(extensions.error());
    }
    for (const auto extension_number : extensions.value()) {
        auto extension = read_mft_record_raw(view, extension_number, cancellation);
        if (!extension) {
            return base::Result<ntfs_core::AttributeValue>::failure(extension.error());
        }
        if (!extension.value().in_use) {
            return shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                           "restore.shrink_unsupported_layout");
        }
        auto status = take(extension.value());
        if (!status) {
            return base::Result<ntfs_core::AttributeValue>::failure(status.error());
        }
    }
    if (!found) {
        return shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                       "restore.shrink_unsupported_layout");
    }
    if (merged.non_resident && merged.runs.size() > 1) {
        auto sorted = sort_and_validate_merged_runs(merged);
        if (!sorted) {
            return base::Result<ntfs_core::AttributeValue>::failure(sorted.error());
        }
    }
    return base::Result<ntfs_core::AttributeValue>::success(std::move(merged));
}

base::Result<ntfs_core::AttributeValue>
load_unnamed_data_attribute(NtfsVolumeView& view, const std::uint64_t record_number,
                            const base::CancellationToken cancellation) {
    return load_unnamed_attribute(view, record_number, ntfs_core::kAttrData, cancellation);
}

base::Result<std::vector<std::byte>>
read_attribute_payload(NtfsVolumeView& view, const ntfs_core::AttributeValue& attribute,
                       const std::uint64_t maximum_bytes,
                       const base::CancellationToken cancellation) {
    if (!attribute.non_resident) {
        return base::Result<std::vector<std::byte>>::success(attribute.resident_data);
    }
    if (attribute.data_size.value > maximum_bytes) {
        return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kUnsupportedVersion,
                                                   "restore.shrink_unsupported_layout");
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(attribute.data_size.value));
    auto n = ntfs_core::read_from_attribute(*view.reader, view.geometry, attribute,
                                            ntfs_core::ByteOffset{0}, std::span<std::byte>(bytes),
                                            cancellation);
    if (!n) {
        return base::Result<std::vector<std::byte>>::failure(n.error());
    }
    if (n.value() != bytes.size()) {
        return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                   "restore.shrink_unsupported_layout");
    }
    return base::Result<std::vector<std::byte>>::success(std::move(bytes));
}

} // namespace aegra::ntfs_resize::detail
