#include "aegra/ntfs_core/attribute_list.h"

#include "aegra/ntfs_core/binary.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace aegra::ntfs_core {

base::Result<std::vector<AttributeListEntry>>
parse_attribute_list(const std::span<const std::byte> list_bytes) {
    std::vector<AttributeListEntry> entries;
    std::size_t offset = 0;
    while (offset + 26 <= list_bytes.size()) {
        if (read_u32(list_bytes, offset) == 0) {
            if (!std::all_of(list_bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                             list_bytes.end(), [](const std::byte value) {
                                 return value == std::byte{0};
                             })) {
                return base::Result<std::vector<AttributeListEntry>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
            }
            break;
        }
        const auto entry_length = read_u16(list_bytes, offset + 4);
        if (entry_length < 26 || offset + entry_length > list_bytes.size()) {
            return base::Result<std::vector<AttributeListEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
        AttributeListEntry entry;
        entry.type = read_u32(list_bytes, offset);
        const auto name_length = std::to_integer<std::uint8_t>(list_bytes[offset + 6]);
        const auto name_offset = std::to_integer<std::uint8_t>(list_bytes[offset + 7]);
        entry.start_vcn.value = read_u64(list_bytes, offset + 8);
        entry.attribute_record = unpack_file_reference(read_u64(list_bytes, offset + 16));
        entry.attribute_id = read_u16(list_bytes, offset + 24);
        if (name_length > 0) {
            std::uint64_t name_bytes = 0;
            if (!checked_mul_u64(name_length, 2, name_bytes) ||
                name_offset + name_bytes > entry_length) {
                return base::Result<std::vector<AttributeListEntry>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
            }
            entry.name.assign(
                reinterpret_cast<const char16_t*>(list_bytes.data() + offset + name_offset),
                name_length);
        }
        entries.push_back(std::move(entry));
        offset += entry_length;
        if (entries.size() > kMaxAttributeListEntries) {
            return base::Result<std::vector<AttributeListEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
    }
    if (!std::all_of(list_bytes.begin() + static_cast<std::ptrdiff_t>(offset), list_bytes.end(),
                     [](const std::byte value) { return value == std::byte{0}; })) {
        return base::Result<std::vector<AttributeListEntry>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    return base::Result<std::vector<AttributeListEntry>>::success(std::move(entries));
}

base::Result<void>
validate_attribute_list_entries(const std::span<const AttributeListEntry> entries,
                                const std::uint64_t /*base_record_number*/) {
    if (entries.size() > kMaxAttributeListEntries) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    // Multi-record Attribute List cycles require I/O and belong to analyzers (SR4+).
    // This codec rejects structurally invalid type values in a single list body.
    for (const auto& entry : entries) {
        if (entry.type == kAttrEnd || entry.type == 0) {
            return base::Result<void>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
    }
    return base::Result<void>::success();
}

} // namespace aegra::ntfs_core
