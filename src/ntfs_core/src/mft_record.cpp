#include "aegra/ntfs_core/mft_record.h"

#include "aegra/ntfs_core/attribute.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/fixup.h"

#include <cstring>
#include <utility>

namespace aegra::ntfs_core {

base::Result<ParsedMftRecord>
parse_mft_record_bytes(const std::span<std::byte> record_bytes,
                       const std::uint32_t bytes_per_sector,
                       const std::uint64_t expected_record_number) {
    if (record_bytes.size() < 48 || record_bytes.size() > kMaxMftRecordBytes) {
        return base::Result<ParsedMftRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
    }
    if (std::memcmp(record_bytes.data(), "FILE", 4) != 0) {
        return base::Result<ParsedMftRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
    }

    const auto usa_offset = read_u16(record_bytes, 4);
    const auto usa_count = read_u16(record_bytes, 6);
    auto fixup = apply_fixup(record_bytes, bytes_per_sector, usa_offset, usa_count);
    if (!fixup) {
        return base::Result<ParsedMftRecord>::failure(fixup.error());
    }

    ParsedMftRecord record;
    record.sequence_number = read_u16(record_bytes, 0x10);
    const auto first_attr = read_u16(record_bytes, 0x14);
    const auto flags = read_u16(record_bytes, 0x16);
    const auto used_size = read_u32(record_bytes, 0x18);
    record.base_record = read_u64(record_bytes, 0x20) & 0x0000FFFFFFFFFFFFULL;
    record.record_number = expected_record_number;
    record.in_use = (flags & kMftRecordInUse) != 0;
    record.is_directory = (flags & kMftRecordIsDirectory) != 0;

    if (used_size < 48 || used_size > record_bytes.size() || first_attr < 48 ||
        first_attr >= used_size) {
        return base::Result<ParsedMftRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
    }

    std::size_t offset = first_attr;
    std::size_t attribute_count = 0;
    while (offset + 8 <= used_size) {
        std::uint32_t type = 0;
        std::uint32_t length = 0;
        bool non_resident = false;
        auto header = validate_attribute_header(record_bytes.subspan(0, used_size), offset, type,
                                                length, non_resident);
        if (!header) {
            return base::Result<ParsedMftRecord>::failure(header.error());
        }
        if (type == kAttrEnd) {
            break;
        }
        auto attribute = parse_attribute(record_bytes.subspan(0, used_size), offset);
        if (!attribute) {
            return base::Result<ParsedMftRecord>::failure(attribute.error());
        }
        record.attributes.push_back(std::move(attribute).value());
        offset += length;
        ++attribute_count;
        if (attribute_count > kMaxAttributesPerRecord) {
            return base::Result<ParsedMftRecord>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
        }
    }
    return base::Result<ParsedMftRecord>::success(std::move(record));
}

} // namespace aegra::ntfs_core
