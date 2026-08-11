#include "ntfs_internal.h"

#include <cstring>

namespace aegra::adapters::ntfs::detail {
namespace {

[[nodiscard]] bool prefer_file_name(const std::uint8_t namespace_type,
                                    const std::uint8_t existing_namespace) noexcept {
    // Prefer Win32 / Win32+DOS over DOS-only.
    const auto score = [](const std::uint8_t ns) noexcept -> int {
        if (ns == kFileNameWin32AndDos || ns == kFileNameWin32) {
            return 2;
        }
        if (ns == 0) { // POSIX
            return 1;
        }
        return 0;
    };
    return score(namespace_type) >= score(existing_namespace);
}

} // namespace

base::Result<ParsedMftRecord>
parse_mft_record_bytes(const std::span<std::byte> record_bytes,
                       const std::uint32_t bytes_per_sector,
                       const std::uint64_t expected_record_number) {
    if (record_bytes.size() < 48 || record_bytes.size() > kMaximumMftRecordBytes) {
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
    constexpr std::size_t kMaxAttributes = 1024;
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
        if (attribute_count > kMaxAttributes) {
            return base::Result<ParsedMftRecord>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
        }
    }
    return base::Result<ParsedMftRecord>::success(std::move(record));
}

base::Result<FileDataLayout>
build_file_layout(const ParsedMftRecord& record,
                  const std::vector<ParsedMftRecord>& extensions) {
    FileDataLayout layout;
    layout.is_directory = record.is_directory;
    std::uint8_t best_name_ns = 0xFF;

    auto consume = [&](const ParsedMftRecord& source) -> base::Result<void> {
        for (const auto& attr : source.attributes) {
            if (attr.type == kAttrStandardInformation && !attr.non_resident &&
                attr.resident_data.size() >= 48) {
                layout.creation_time = read_u64(attr.resident_data, 0);
                layout.modification_time = read_u64(attr.resident_data, 8);
                layout.mft_change_time = read_u64(attr.resident_data, 16);
                layout.access_time = read_u64(attr.resident_data, 24);
                layout.file_attributes = read_u32(attr.resident_data, 32);
                layout.is_reparse = (layout.file_attributes & kFileAttrReparse) != 0;
                layout.is_compressed = (layout.file_attributes & kFileAttrCompressed) != 0 ||
                                       attr.compressed;
                layout.is_encrypted = (layout.file_attributes & kFileAttrEncrypted) != 0 ||
                                      attr.encrypted;
            } else if (attr.type == kAttrFileName && !attr.non_resident &&
                       attr.resident_data.size() >= 66) {
                const auto name_length = std::to_integer<std::uint8_t>(attr.resident_data[64]);
                const auto name_ns = std::to_integer<std::uint8_t>(attr.resident_data[65]);
                if (66U + static_cast<std::size_t>(name_length) * 2U > attr.resident_data.size()) {
                    return base::Result<void>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
                }
                if (layout.best_name.empty() || prefer_file_name(name_ns, best_name_ns)) {
                    best_name_ns = name_ns;
                    layout.best_name.assign(
                        reinterpret_cast<const char16_t*>(attr.resident_data.data() + 66),
                        name_length);
                    const auto parent = read_u64(attr.resident_data, 0);
                    layout.parent_record = parent & 0x0000FFFFFFFFFFFFULL;
                    layout.parent_sequence = static_cast<std::uint16_t>(parent >> 48);
                }
            } else if (attr.type == kAttrData && attr.name.empty()) {
                layout.has_unnamed_data = true;
                layout.unnamed_data = attr;
                layout.logical_size = attr.data_size;
                layout.allocated_size = attr.allocated_size;
                layout.is_compressed = layout.is_compressed || attr.compressed;
                layout.is_encrypted = layout.is_encrypted || attr.encrypted;
            } else if (attr.type == kAttrIndexRoot &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_root = true;
                layout.index_root = attr;
            } else if (attr.type == kAttrIndexAllocation &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_allocation = true;
                layout.index_allocation = attr;
            } else if (attr.type == kAttrBitmap &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_bitmap = true;
                layout.index_bitmap = attr;
            }
        }
        return base::Result<void>::success();
    };

    auto base = consume(record);
    if (!base) {
        return base::Result<FileDataLayout>::failure(base.error());
    }
    for (const auto& extension : extensions) {
        auto ext = consume(extension);
        if (!ext) {
            return base::Result<FileDataLayout>::failure(ext.error());
        }
    }
    if ((layout.file_attributes & kFileAttrDirectory) != 0) {
        layout.is_directory = true;
    }
    return base::Result<FileDataLayout>::success(std::move(layout));
}

} // namespace aegra::adapters::ntfs::detail
