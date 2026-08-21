#include "aegra/ntfs_core/attribute.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/runlist.h"

#include <cstring>
#include <utility>

namespace aegra::ntfs_core {
namespace {

[[nodiscard]] base::Result<std::u16string>
read_utf16_name(const std::span<const std::byte> data, const std::size_t offset,
                const std::size_t char_count) {
    std::uint64_t byte_count = 0;
    if (!checked_mul_u64(char_count, 2, byte_count) || offset + byte_count > data.size()) {
        return base::Result<std::u16string>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    std::u16string name(char_count, u'\0');
    std::memcpy(name.data(), data.data() + offset, static_cast<std::size_t>(byte_count));
    return base::Result<std::u16string>::success(std::move(name));
}

} // namespace

bool is_known_attribute_type(const std::uint32_t type) noexcept {
    switch (type) {
    case kAttrStandardInformation:
    case kAttrAttributeList:
    case kAttrFileName:
    case 0x40:
    case 0x50:
    case 0x60:
    case 0x70:
    case kAttrData:
    case kAttrIndexRoot:
    case kAttrIndexAllocation:
    case kAttrBitmap:
    case 0xC0:
    case 0xD0:
    case 0xE0:
    case 0xF0:
    case 0x100:
    case kAttrEnd:
        return true;
    default:
        return false;
    }
}

base::Result<void> validate_attribute_header(const std::span<const std::byte> record,
                                             const std::size_t offset, std::uint32_t& type,
                                             std::uint32_t& length, bool& non_resident) {
    if (offset > record.size() || record.size() - offset < 8) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    type = read_u32(record, offset);
    if (type == kAttrEnd) {
        length = 0;
        non_resident = false;
        return base::Result<void>::success();
    }
    if (record.size() - offset < 16) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    length = read_u32(record, offset + 4);
    if (length < 16 || length > record.size() - offset) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    non_resident = std::to_integer<std::uint8_t>(record[offset + 8]) != 0;
    return base::Result<void>::success();
}

base::Result<AttributeValue> parse_attribute(const std::span<const std::byte> record,
                                             const std::size_t offset) {
    std::uint32_t type = 0;
    std::uint32_t length = 0;
    bool non_resident = false;
    auto header = validate_attribute_header(record, offset, type, length, non_resident);
    if (!header) {
        return base::Result<AttributeValue>::failure(header.error());
    }
    if (type == kAttrEnd) {
        return base::Result<AttributeValue>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.attribute_out_of_bounds"));
    }

    const auto attr = record.subspan(offset, length);
    AttributeValue value;
    value.type = type;
    value.record_offset = static_cast<std::uint32_t>(offset);
    value.attribute_length = length;
    value.non_resident = non_resident;
    const auto name_length = std::to_integer<std::uint8_t>(attr[9]);
    const auto name_offset = read_u16(attr, 10);
    const auto flags = read_u16(attr, 12);
    value.attribute_id = read_u16(attr, 14);
    value.compressed = (flags & 0x0001) != 0;
    value.encrypted = (flags & 0x4000) != 0;
    value.sparse = (flags & 0x8000) != 0;

    if (name_length > 0) {
        if (name_offset + static_cast<std::size_t>(name_length) * 2U > attr.size()) {
            return base::Result<AttributeValue>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
        auto name = read_utf16_name(attr, name_offset, name_length);
        if (!name) {
            return base::Result<AttributeValue>::failure(name.error());
        }
        value.name = std::move(name).value();
    }

    if (!non_resident) {
        if (attr.size() < 24) {
            return base::Result<AttributeValue>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
        const auto value_length = read_u32(attr, 16);
        const auto value_offset = read_u16(attr, 20);
        if (value_offset + static_cast<std::uint64_t>(value_length) > attr.size()) {
            return base::Result<AttributeValue>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
        }
        value.data_size.value = value_length;
        value.allocated_size.value = value_length;
        value.initialized_size.value = value_length;
        value.resident_data.assign(attr.begin() + value_offset,
                                   attr.begin() + value_offset + value_length);
        return base::Result<AttributeValue>::success(std::move(value));
    }

    if (attr.size() < 64) {
        return base::Result<AttributeValue>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    const VirtualClusterNumber start_vcn{read_u64(attr, 16)};
    const VirtualClusterNumber last_vcn{read_u64(attr, 24)};
    const auto runlist_offset = read_u16(attr, 32);
    value.runlist_offset = runlist_offset;
    const auto compression_unit = read_u16(attr, 34);
    value.allocated_size.value = read_u64(attr, 40);
    value.data_size.value = read_u64(attr, 48);
    value.initialized_size.value = read_u64(attr, 56);
    if (compression_unit != 0 || value.compressed) {
        value.compressed = true;
    }
    if (runlist_offset >= attr.size()) {
        return base::Result<AttributeValue>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
    }
    auto runs = parse_runlist(attr.subspan(runlist_offset), start_vcn, last_vcn);
    if (!runs) {
        return base::Result<AttributeValue>::failure(runs.error());
    }
    value.runs = std::move(runs).value();
    return base::Result<AttributeValue>::success(std::move(value));
}

} // namespace aegra::ntfs_core
