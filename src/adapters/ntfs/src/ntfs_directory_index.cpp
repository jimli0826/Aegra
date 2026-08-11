#include "ntfs_internal.h"

#include <cstring>

namespace aegra::adapters::ntfs::detail {

base::Result<std::vector<NtfsEntry>>
parse_index_entries(const std::span<const std::byte> entry_area) {
    std::vector<NtfsEntry> entries;
    std::size_t offset = 0;
    constexpr std::size_t kMaxEntries = 1'000'000;

    while (offset + 16 <= entry_area.size()) {
        const auto entry_length = read_u16(entry_area, offset + 8);
        const auto key_length = read_u16(entry_area, offset + 10);
        const auto flags = read_u32(entry_area, offset + 12);
        if (entry_length < 16 || offset + entry_length > entry_area.size()) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
        }
        if ((flags & kIndexEntryEnd) != 0) {
            break;
        }
        if (key_length >= 66 && 16U + key_length <= entry_length) {
            const auto key = entry_area.subspan(offset + 16, key_length);
            const auto packed_ref = read_u64(entry_area, offset);
            const auto reference = unpack_file_reference(packed_ref);
            // Skip self/parent pseudo entries often seen as "." and ".."
            const auto name_length = std::to_integer<std::uint8_t>(key[64]);
            const auto name_ns = std::to_integer<std::uint8_t>(key[65]);
            if (66U + static_cast<std::size_t>(name_length) * 2U > key.size()) {
                return base::Result<std::vector<NtfsEntry>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
            }
            std::u16string name(reinterpret_cast<const char16_t*>(key.data() + 66), name_length);
            if (name == u"." || name == u"..") {
                offset += entry_length;
                continue;
            }
            // Prefer Win32 names; skip pure DOS short names when a Win32 name will appear.
            if (name_ns == 2 /* DOS */) {
                offset += entry_length;
                continue;
            }

            NtfsEntry entry;
            entry.reference = reference;
            entry.name = std::move(name);
            // $FILE_NAME: allocated@0x28, real@0x30, flags@0x38.
            entry.allocated_size = read_u64(key, 40);
            entry.logical_size = read_u64(key, 48);
            entry.file_attributes = read_u32(key, 56);
            entry.creation_time = read_u64(key, 8);
            entry.modification_time = read_u64(key, 16);
            entry.mft_change_time = read_u64(key, 24);
            entry.access_time = read_u64(key, 32);
            apply_file_name_flags(entry.file_attributes, entry.is_directory);
            entry.is_reparse = (entry.file_attributes & kFileAttrReparse) != 0;
            entry.is_compressed = (entry.file_attributes & kFileAttrCompressed) != 0;
            entry.is_encrypted = (entry.file_attributes & kFileAttrEncrypted) != 0;
            entry.is_hidden = (entry.file_attributes & kFileAttrHidden) != 0;
            entry.is_system = (entry.file_attributes & kFileAttrSystem) != 0;
            entries.push_back(std::move(entry));
        }
        if (entry_length == 0) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
        }
        offset += entry_length;
        if (entries.size() > kMaxEntries) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
        }
    }
    return base::Result<std::vector<NtfsEntry>>::success(std::move(entries));
}

} // namespace aegra::adapters::ntfs::detail
