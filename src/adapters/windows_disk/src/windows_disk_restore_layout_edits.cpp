#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

[[nodiscard]] std::uint32_t crc32_ieee(const std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

void write_le32(std::vector<std::byte>& buffer, const std::size_t offset,
                const std::uint32_t value) {
    if (offset + 4 > buffer.size()) {
        return;
    }
    buffer[offset] = static_cast<std::byte>(value & 0xFFU);
    buffer[offset + 1] = static_cast<std::byte>((value >> 8U) & 0xFFU);
    buffer[offset + 2] = static_cast<std::byte>((value >> 16U) & 0xFFU);
    buffer[offset + 3] = static_cast<std::byte>((value >> 24U) & 0xFFU);
}

void write_le64(std::vector<std::byte>& buffer, const std::size_t offset,
                const std::uint64_t value) {
    if (offset + 8 > buffer.size()) {
        return;
    }
    for (int i = 0; i < 8; ++i) {
        buffer[offset + static_cast<std::size_t>(i)] =
            static_cast<std::byte>((value >> (8 * i)) & 0xFFU);
    }
}

[[nodiscard]] std::uint32_t read_le32(const std::byte* data) noexcept {
    std::uint32_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] std::uint64_t read_le64(const std::byte* data) noexcept {
    std::uint64_t value = 0;
    std::memcpy(&value, data, sizeof(value));
    return value;
}

[[nodiscard]] bool is_zero_guid_bytes(const std::byte* data) noexcept {
    for (std::size_t i = 0; i < 16; ++i) {
        if (data[i] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

constexpr GUID kGptBasicDataPartitionType{0xEBD0A0A2,
                                          0xB9E5,
                                          0x4433,
                                          {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

[[nodiscard]] bool guid_equal(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

[[nodiscard]] bool is_mbr_structure_or_system_type(const std::uint8_t type) noexcept {
    switch (type) {
    case 0x05:
    case 0x0F:
    case 0x12:
    case 0x27:
    case 0xDE:
    case 0xEE:
    case 0xEF:
        return true;
    default:
        return false;
    }
}

void recompute_gpt_header_crcs(std::vector<std::byte>& header,
                               const std::span<const std::byte> partition_entry_array) {
    if (header.size() < 92 || partition_entry_array.empty()) {
        return;
    }
    write_le32(header, 88, crc32_ieee(partition_entry_array));
    write_le32(header, 16, 0);
    std::uint32_t header_size = read_le32(header.data() + 12);
    if (header_size < 92 || header_size > header.size()) {
        header_size = 92;
    }
    const auto crc = crc32_ieee(
        std::span<const std::byte>(header.data(), static_cast<std::size_t>(header_size)));
    write_le32(header, 16, crc);
}

// UEFI GPT header LBA fields (little-endian):
//   24 MyLBA, 32 AlternateLBA, 40 FirstUsableLBA, 48 LastUsableLBA, 72 PartitionEntryLBA
enum class GptHeaderRole : std::uint8_t { Primary, Backup };

// Rewrite header LBAs for the target disk. Primary and backup headers differ:
// primary: MyLBA=1, AlternateLBA=last, PartitionEntryLBA=2
// backup:  MyLBA=last, AlternateLBA=1, PartitionEntryLBA=last−entry_array_sectors
void update_gpt_header_geometry_for_target(std::vector<std::byte>& header,
                                           const std::uint32_t sector_size,
                                           const std::uint64_t target_disk_size_bytes,
                                           const std::uint64_t array_bytes,
                                           const GptHeaderRole role) {
    if (header.size() < 92 || target_disk_size_bytes < static_cast<std::uint64_t>(sector_size) * 2U) {
        return;
    }
    const auto sector = sector_size == 0 ? 512U : sector_size;
    const auto last_lba = target_disk_size_bytes / sector - 1ULL;
    auto entry_sectors =
        (array_bytes + static_cast<std::uint64_t>(sector) - 1ULL) / sector;
    if (entry_sectors == 0) {
        entry_sectors = 32; // 128×128 standard array
    }
    // Primary entries at LBA 2; usable range after that array until before backup array.
    const auto first_usable = 2ULL + entry_sectors;
    const auto last_usable =
        last_lba > entry_sectors + 1ULL ? last_lba - entry_sectors - 1ULL : first_usable;
    if (role == GptHeaderRole::Primary) {
        write_le64(header, 24, 1ULL);                  // MyLBA
        write_le64(header, 32, last_lba);              // AlternateLBA → backup header
        write_le64(header, 40, first_usable);          // FirstUsableLBA
        write_le64(header, 48, last_usable);           // LastUsableLBA
        write_le64(header, 72, 2ULL);                  // PartitionEntryLBA
    } else {
        const auto backup_entry_lba = last_lba > entry_sectors ? last_lba - entry_sectors : 2ULL;
        write_le64(header, 24, last_lba);              // MyLBA → end of disk
        write_le64(header, 32, 1ULL);                  // AlternateLBA → primary header
        write_le64(header, 40, first_usable);          // FirstUsableLBA (same as primary)
        write_le64(header, 48, last_usable);           // LastUsableLBA
        write_le64(header, 72, backup_entry_lba);      // PartitionEntryLBA → trailing array
    }
}

struct ByteRange final {
    std::uint64_t start{0};
    std::uint64_t end{0}; // exclusive
};

struct GptEditResolveContext final {
    const std::vector<std::byte>* entries{nullptr};
    std::uint32_t entry_size{0};
    std::uint32_t entry_count{0};
    std::uint32_t sector{0};
    std::uint64_t max_end{0};
    std::uint64_t cursor{0};
    const std::vector<ByteRange>* reserved{nullptr};
};

struct MbrEditResolveContext final {
    const std::vector<std::byte>* mbr{nullptr};
    std::uint32_t sector{0};
    std::uint64_t usable_end{0};
    std::uint64_t cursor{0};
    const std::vector<ByteRange>* reserved{nullptr};
};

struct GptPartitionLayoutApply final {
    std::vector<std::byte>* entries{nullptr};
    std::vector<std::byte>* header{nullptr};
    std::uint32_t sector_size{0};
    std::uint64_t target_disk_size_bytes{0};
    std::span<const PartitionLayoutEdit> edits{};
    GptHeaderRole role{GptHeaderRole::Primary};
};

constexpr std::size_t kMaximumPartitionLayoutEdits = 128;
constexpr std::size_t kMbrTableOffset = 0x1BE;
constexpr std::size_t kMbrEntrySize = 16;
constexpr char kGptSignature[8] = {'E', 'F', 'I', ' ', 'P', 'A', 'R', 'T'};

[[nodiscard]] bool checked_add_u64(const std::uint64_t a, const std::uint64_t b,
                                   std::uint64_t& out) noexcept {
    if (a > (std::numeric_limits<std::uint64_t>::max)() - b) {
        return false;
    }
    out = a + b;
    return true;
}

[[nodiscard]] bool checked_mul_u64(const std::uint64_t a, const std::uint64_t b,
                                   std::uint64_t& out) noexcept {
    if (a == 0 || b == 0) {
        out = 0;
        return true;
    }
    if (a > (std::numeric_limits<std::uint64_t>::max)() / b) {
        return false;
    }
    out = a * b;
    return true;
}

[[nodiscard]] base::Result<void>
verify_mbr_boot_signature(const std::vector<std::byte>& mbr) {
    if (mbr.size() < 512) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR sector is incomplete"});
    }
    if (mbr[510] != std::byte{0x55} || mbr[511] != std::byte{0xAA}) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "MBR boot signature is invalid"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
reject_mbr_extended_partitions(const std::vector<std::byte>& mbr) {
    if (mbr.size() < kMbrTableOffset + kMbrEntrySize * 4) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR partition table is incomplete"});
    }
    for (std::size_t i = 0; i < 4; ++i) {
        const auto type =
            static_cast<std::uint8_t>(mbr[kMbrTableOffset + i * kMbrEntrySize + 4]);
        if (type == 0x05 || type == 0x0F) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument,
                 "MBR extended/logical partitions are not supported for disk restore"});
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
verify_gpt_header_fields(const std::vector<std::byte>& header,
                         const std::span<const std::byte> entries, const std::uint32_t sector) {
    if (header.size() < 92) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT header is incomplete"});
    }
    if (std::memcmp(header.data(), kGptSignature, sizeof(kGptSignature)) != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT header signature is invalid"});
    }
    const auto header_size = read_le32(header.data() + 12);
    if (header_size < 92U || header_size > sector ||
        static_cast<std::size_t>(header_size) > header.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT header size is invalid"});
    }
    const auto entry_size = read_le32(header.data() + 84);
    const auto entry_count = read_le32(header.data() + 80);
    std::uint64_t array_bytes = 0;
    if (entry_size < 128 || entry_count == 0 ||
        !checked_mul_u64(entry_size, entry_count, array_bytes) ||
        array_bytes > entries.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT partition entry array is invalid"});
    }
    const auto stored_array_crc = read_le32(header.data() + 88);
    const auto array_span =
        std::span<const std::byte>(entries.data(), static_cast<std::size_t>(array_bytes));
    if (crc32_ieee(array_span) != stored_array_crc) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT partition entry array CRC mismatch"});
    }
    const auto stored_header_crc = read_le32(header.data() + 16);
    std::vector<std::byte> prefix(header.begin(),
                                  header.begin() + static_cast<std::ptrdiff_t>(header_size));
    write_le32(prefix, 16, 0);
    if (crc32_ieee(std::span<const std::byte>(prefix.data(), prefix.size())) !=
        stored_header_crc) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT header CRC mismatch"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
verify_gpt_primary_backup_agreement(const std::vector<std::byte>& primary,
                                    const std::vector<std::byte>& backup) {
    if (primary.size() < 92 || backup.size() < 92) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT header is incomplete"});
    }
    if (read_le32(primary.data() + 80) != read_le32(backup.data() + 80) ||
        read_le32(primary.data() + 84) != read_le32(backup.data() + 84)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData,
             "GPT primary/backup partition entry geometry mismatch"});
    }
    const auto primary_my = read_le64(primary.data() + 24);
    const auto primary_alt = read_le64(primary.data() + 32);
    const auto backup_my = read_le64(backup.data() + 24);
    const auto backup_alt = read_le64(backup.data() + 32);
    if (primary_alt != backup_my || backup_alt != primary_my) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData,
             "GPT primary/backup MyLBA/AlternateLBA mismatch"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_layout_edit_inputs(const std::span<const PartitionLayoutEdit> edits,
                            const std::uint32_t sector, const bool mbr_field_limits) {
    if (edits.size() > kMaximumPartitionLayoutEdits) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout edit count exceeds limit"});
    }
    std::vector<std::uint64_t> sources;
    sources.reserve(edits.size());
    for (const auto& edit : edits) {
        if (edit.size_bytes == 0) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition layout size is zero"});
        }
        if (sector != 0 &&
            ((edit.target_start_offset_bytes % sector) != 0 || (edit.size_bytes % sector) != 0)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition layout edit is not sector-aligned"});
        }
        std::uint64_t end = 0;
        if (!checked_add_u64(edit.target_start_offset_bytes, edit.size_bytes, end)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition layout target range overflows"});
        }
        if (mbr_field_limits && sector != 0) {
            const auto first_lba = edit.target_start_offset_bytes / sector;
            const auto count = edit.size_bytes / sector;
            if (first_lba > (std::numeric_limits<std::uint32_t>::max)() ||
                count > (std::numeric_limits<std::uint32_t>::max)()) {
                return base::Result<void>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "MBR partition LBA or sector count exceeds uint32 range"});
            }
        }
        sources.push_back(edit.source_start_offset_bytes);
    }
    std::sort(sources.begin(), sources.end());
    for (std::size_t i = 1; i < sources.size(); ++i) {
        if (sources[i] == sources[i - 1]) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "duplicate partition layout source start"});
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_final_intervals(std::vector<ByteRange> intervals, const std::uint64_t usable_start,
                         const std::uint64_t usable_end) {
    for (const auto& r : intervals) {
        if (r.start >= r.end) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition interval is empty or inverted"});
        }
        if (r.start < usable_start || r.end > usable_end) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition interval outside usable range"});
        }
    }
    std::sort(intervals.begin(), intervals.end(),
              [](const ByteRange& a, const ByteRange& b) { return a.start < b.start; });
    for (std::size_t i = 1; i < intervals.size(); ++i) {
        if (intervals[i].start < intervals[i - 1].end) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "partition intervals overlap"});
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::uint64_t gpt_max_partition_end_bytes(const std::uint32_t sector,
                                                        const std::uint64_t target_disk_size_bytes,
                                                        const std::uint64_t array_bytes) noexcept {
    if (target_disk_size_bytes < static_cast<std::uint64_t>(sector) * 2U) {
        return target_disk_size_bytes;
    }
    const auto last_lba = target_disk_size_bytes / sector - 1ULL;
    auto entry_sectors =
        (array_bytes + static_cast<std::uint64_t>(sector) - 1ULL) / sector;
    if (entry_sectors == 0) {
        entry_sectors = 32;
    }
    const auto last_usable =
        last_lba > entry_sectors + 1ULL ? last_lba - entry_sectors - 1ULL : 34ULL;
    std::uint64_t max_end = 0;
    if (!checked_mul_u64(last_usable + 1ULL, sector, max_end)) {
        return 0;
    }
    constexpr std::uint64_t kGptTailReserve = 1024ULL * 1024ULL;
    if (target_disk_size_bytes > kGptTailReserve) {
        const auto reserved_end = target_disk_size_bytes - kGptTailReserve;
        if (reserved_end < max_end) {
            max_end = reserved_end;
        }
    }
    return (max_end / sector) * static_cast<std::uint64_t>(sector);
}

// Snap preferred [start,start+size) away from fixed reserved ranges and max_end.
// UI starts/sizes are hints; this is the authoritative placement clamp.
void clamp_range_from_reserved(std::uint64_t& start, std::uint64_t& size,
                               const std::uint64_t max_end, const std::uint32_t sector,
                               const std::vector<ByteRange>& reserved) {
    if (size == 0 || sector == 0) {
        return;
    }
    start = (start / sector) * static_cast<std::uint64_t>(sector);
    size = (size / sector) * static_cast<std::uint64_t>(sector);
    if (max_end > 0 && start >= max_end) {
        size = 0;
        return;
    }
    for (std::size_t guard = 0; guard < reserved.size() + 2U; ++guard) {
        bool moved = false;
        for (const auto& r : reserved) {
            std::uint64_t end = 0;
            if (!checked_add_u64(start, size, end)) {
                size = 0;
                return;
            }
            if (start < r.end && end > r.start) {
                start = ((r.end + sector - 1U) / sector) * static_cast<std::uint64_t>(sector);
                moved = true;
            }
        }
        if (!moved) {
            break;
        }
    }
    if (max_end > 0 && start >= max_end) {
        size = 0;
        return;
    }
    auto limit = max_end > 0 ? max_end : (std::numeric_limits<std::uint64_t>::max)();
    for (const auto& r : reserved) {
        if (r.start > start && r.start < limit) {
            limit = r.start;
        }
    }
    std::uint64_t end = 0;
    if (!checked_add_u64(start, size, end) || end > limit) {
        size = limit > start ? ((limit - start) / sector) * static_cast<std::uint64_t>(sector) : 0;
    }
}

[[nodiscard]] std::vector<ByteRange>
collect_gpt_reserved_ranges(const std::vector<std::byte>& entries, const std::uint32_t entry_size,
                            const std::uint32_t entry_count, const std::uint32_t sector) {
    std::vector<ByteRange> reserved;
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const auto* entry = entries.data() + static_cast<std::size_t>(i) * entry_size;
        if (is_zero_guid_bytes(entry)) {
            continue;
        }
        GUID type{};
        std::memcpy(&type, entry, sizeof(type));
        if (guid_equal(type, kGptBasicDataPartitionType)) {
            continue;
        }
        const auto first = read_le64(entry + 32);
        const auto last = read_le64(entry + 40);
        if (last < first) {
            continue;
        }
        reserved.push_back({first * static_cast<std::uint64_t>(sector),
                            (last + 1ULL) * static_cast<std::uint64_t>(sector)});
    }
    std::sort(reserved.begin(), reserved.end(),
              [](const ByteRange& a, const ByteRange& b) { return a.start < b.start; });
    return reserved;
}

[[nodiscard]] base::Result<void>
validate_gpt_final_layout(const std::vector<std::byte>& entries, const std::vector<std::byte>& header,
                          const std::uint32_t sector, const std::uint64_t target_disk_size_bytes,
                          const std::span<const PartitionLayoutEdit> resolved_edits) {
    if (header.size() < 92 || entries.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT layout is incomplete for validation"});
    }
    const auto entry_size = read_le32(header.data() + 84);
    const auto entry_count = read_le32(header.data() + 80);
    const auto array_bytes =
        static_cast<std::uint64_t>(entry_size) * static_cast<std::uint64_t>(entry_count);
    if (entry_size < 128 || entry_count == 0 || array_bytes > entries.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT partition entry array is invalid"});
    }
    auto entry_sectors =
        (array_bytes + static_cast<std::uint64_t>(sector) - 1ULL) / sector;
    if (entry_sectors == 0) {
        entry_sectors = 32;
    }
    const auto usable_start = (2ULL + entry_sectors) * static_cast<std::uint64_t>(sector);
    const auto usable_end =
        gpt_max_partition_end_bytes(sector, target_disk_size_bytes, array_bytes);
    if (usable_end <= usable_start) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT usable range is empty"});
    }
    std::vector<ByteRange> intervals;
    intervals.reserve(entry_count);
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const auto* entry = entries.data() + static_cast<std::size_t>(i) * entry_size;
        if (is_zero_guid_bytes(entry)) {
            continue;
        }
        GUID type{};
        std::memcpy(&type, entry, sizeof(type));
        const auto first = read_le64(entry + 32);
        const auto last = read_le64(entry + 40);
        if (last < first) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCorruptData, "GPT partition LBA range is inverted"});
        }
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        if (!checked_mul_u64(first, sector, start) ||
            !checked_mul_u64(last + 1ULL, sector, end)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "GPT partition LBA range overflows"});
        }
        if (guid_equal(type, kGptBasicDataPartitionType)) {
            for (const auto& edit : resolved_edits) {
                const auto delta = start >= edit.source_start_offset_bytes
                                       ? start - edit.source_start_offset_bytes
                                       : edit.source_start_offset_bytes - start;
                if (delta > sector) {
                    continue;
                }
                start = edit.target_start_offset_bytes;
                if (!checked_add_u64(start, edit.size_bytes, end)) {
                    return base::Result<void>::failure(
                        {base::ErrorCode::kInvalidArgument,
                         "resolved GPT partition range overflows"});
                }
                break;
            }
        }
        intervals.push_back({start, end});
    }
    return validate_final_intervals(std::move(intervals), usable_start, usable_end);
}

[[nodiscard]] base::Result<std::uint64_t>
match_gpt_basic_data_source_size(const std::vector<std::byte>& entries,
                                 const std::uint32_t entry_size, const std::uint32_t entry_count,
                                 const std::uint32_t sector, const std::uint64_t source_start) {
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        const auto* entry = entries.data() + static_cast<std::size_t>(i) * entry_size;
        if (is_zero_guid_bytes(entry)) {
            continue;
        }
        GUID type{};
        std::memcpy(&type, entry, sizeof(type));
        if (!guid_equal(type, kGptBasicDataPartitionType)) {
            continue;
        }
        const auto first = read_le64(entry + 32);
        const auto last = read_le64(entry + 40);
        std::uint64_t start = 0;
        if (!checked_mul_u64(first, sector, start)) {
            return base::Result<std::uint64_t>::failure(
                {base::ErrorCode::kCorruptData, "GPT source partition start overflows"});
        }
        const auto delta =
            start >= source_start ? start - source_start : source_start - start;
        if (delta > sector) {
            continue;
        }
        std::uint64_t source_size = 0;
        if (last >= first) {
            source_size = (last - first + 1ULL) * static_cast<std::uint64_t>(sector);
        }
        return base::Result<std::uint64_t>::success(source_size);
    }
    return base::Result<std::uint64_t>::failure(
        {base::ErrorCode::kNotFound, "partition layout edit source start not found"});
}

[[nodiscard]] base::Result<PartitionLayoutEdit>
resolve_one_gpt_edit(const GptEditResolveContext& ctx, const PartitionLayoutEdit& edit) {
    if (ctx.entries == nullptr || ctx.reserved == nullptr || ctx.sector == 0) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT edit resolve context is incomplete"});
    }
    auto source_size = match_gpt_basic_data_source_size(
        *ctx.entries, ctx.entry_size, ctx.entry_count, ctx.sector,
        edit.source_start_offset_bytes);
    if (!source_size) {
        return base::Result<PartitionLayoutEdit>::failure(source_size.error());
    }
    auto start =
        (edit.target_start_offset_bytes / ctx.sector) * static_cast<std::uint64_t>(ctx.sector);
    auto size = (edit.size_bytes / ctx.sector) * static_cast<std::uint64_t>(ctx.sector);
    if (size == 0) {
        size = (source_size.value() / ctx.sector) * static_cast<std::uint64_t>(ctx.sector);
    }
    if (start < ctx.cursor) {
        start = ctx.cursor;
    }
    clamp_range_from_reserved(start, size, ctx.max_end, ctx.sector, *ctx.reserved);
    if (size == 0) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument,
             "partition layout has no free space after reserved ranges"});
    }
    std::uint64_t placed_end = 0;
    if (!checked_add_u64(start, size, placed_end)) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument, "resolved partition end overflows"});
    }
    return base::Result<PartitionLayoutEdit>::success(
        PartitionLayoutEdit{edit.source_start_offset_bytes, start, size});
}

[[nodiscard]] base::Result<std::vector<PartitionLayoutEdit>>
resolve_gpt_layout_edits(const std::vector<std::byte>& entries, const std::vector<std::byte>& header,
                         const std::uint32_t sector_size,
                         const std::uint64_t target_disk_size_bytes,
                         const std::span<const PartitionLayoutEdit> edits) {
    std::vector<PartitionLayoutEdit> out;
    if (header.size() < 92 || entries.empty() || edits.empty()) {
        return base::Result<std::vector<PartitionLayoutEdit>>::success(std::move(out));
    }
    const auto sector = sector_size == 0 ? 512U : sector_size;
    auto inputs = validate_layout_edit_inputs(edits, sector, false);
    if (!inputs) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(inputs.error());
    }
    const auto entry_size = read_le32(header.data() + 84);
    const auto entry_count = read_le32(header.data() + 80);
    const auto array_bytes =
        static_cast<std::uint64_t>(entry_size) * static_cast<std::uint64_t>(entry_count);
    if (entry_size < 128 || entry_count == 0 || array_bytes > entries.size()) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(
            {base::ErrorCode::kCorruptData, "GPT partition entry array is invalid"});
    }
    const auto max_end =
        gpt_max_partition_end_bytes(sector, target_disk_size_bytes, array_bytes);
    const auto reserved =
        collect_gpt_reserved_ranges(entries, entry_size, entry_count, sector);
    std::vector<PartitionLayoutEdit> ordered(edits.begin(), edits.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const PartitionLayoutEdit& a, const PartitionLayoutEdit& b) {
                  return a.source_start_offset_bytes < b.source_start_offset_bytes;
              });
    GptEditResolveContext ctx{&entries, entry_size, entry_count, sector, max_end, 0, &reserved};
    out.reserve(ordered.size());
    for (const auto& edit : ordered) {
        auto placed = resolve_one_gpt_edit(ctx, edit);
        if (!placed) {
            return base::Result<std::vector<PartitionLayoutEdit>>::failure(placed.error());
        }
        ctx.cursor = placed.value().target_start_offset_bytes + placed.value().size_bytes;
        out.push_back(std::move(placed).value());
    }
    auto final_ok =
        validate_gpt_final_layout(entries, header, sector, target_disk_size_bytes, out);
    if (!final_ok) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(final_ok.error());
    }
    return base::Result<std::vector<PartitionLayoutEdit>>::success(std::move(out));
}

// Data partitions must not claim LBA 0 (MBR itself). Prefer at least 1 MiB when disk allows.
[[nodiscard]] std::uint64_t mbr_min_data_start_bytes(const std::uint32_t sector) noexcept {
    constexpr std::uint64_t kPreferMiB = 1024ULL * 1024ULL;
    if (sector == 0) {
        return kPreferMiB;
    }
    if (kPreferMiB % sector == 0) {
        return kPreferMiB;
    }
    return static_cast<std::uint64_t>(sector); // LBA 1
}

[[nodiscard]] std::uint64_t mbr_max_addressable_end_bytes(const std::uint32_t sector) noexcept {
    // Exclusive end of the classic MBR 32-bit LBA space.
    return (static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)()) + 1ULL) *
           static_cast<std::uint64_t>(sector);
}

[[nodiscard]] std::uint64_t mbr_usable_end_bytes(const std::uint32_t sector,
                                                 const std::uint64_t target_disk_size_bytes) noexcept {
    const auto addressable = mbr_max_addressable_end_bytes(sector);
    if (target_disk_size_bytes == 0) {
        return addressable;
    }
    return target_disk_size_bytes < addressable ? target_disk_size_bytes : addressable;
}

[[nodiscard]] bool mbr_lba_fields_ok(const std::uint64_t first_lba,
                                     const std::uint64_t count) noexcept {
    return first_lba > 0 && count > 0 &&
           first_lba <= (std::numeric_limits<std::uint32_t>::max)() &&
           count <= (std::numeric_limits<std::uint32_t>::max)();
}

[[nodiscard]] base::Result<ByteRange>
mbr_entry_to_range(const std::uint64_t first_lba, const std::uint64_t count,
                   const std::uint32_t sector) {
    if (!mbr_lba_fields_ok(first_lba, count)) {
        return base::Result<ByteRange>::failure(
            {base::ErrorCode::kInvalidArgument,
             "MBR partition LBA or sector count out of range"});
    }
    std::uint64_t start = 0;
    std::uint64_t end = 0;
    if (!checked_mul_u64(first_lba, sector, start) ||
        !checked_mul_u64(first_lba + count, sector, end)) {
        return base::Result<ByteRange>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR partition range overflows"});
    }
    return base::Result<ByteRange>::success(ByteRange{start, end});
}

[[nodiscard]] std::vector<ByteRange>
collect_mbr_reserved_ranges(const std::vector<std::byte>& mbr, const std::uint32_t sector) {
    constexpr std::size_t kTable = 0x1BE;
    constexpr std::size_t kEntry = 16;
    std::vector<ByteRange> reserved;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* entry = mbr.data() + kTable + i * kEntry;
        const auto type = static_cast<std::uint8_t>(entry[4]);
        if (type == 0 || !is_mbr_structure_or_system_type(type)) {
            continue;
        }
        const auto first = static_cast<std::uint64_t>(read_le32(entry + 8));
        const auto count = static_cast<std::uint64_t>(read_le32(entry + 12));
        if (count == 0) {
            continue;
        }
        std::uint64_t start = 0;
        std::uint64_t end = 0;
        if (!checked_mul_u64(first, sector, start) ||
            !checked_mul_u64(first + count, sector, end)) {
            continue;
        }
        reserved.push_back({start, end});
    }
    std::sort(reserved.begin(), reserved.end(),
              [](const ByteRange& a, const ByteRange& b) { return a.start < b.start; });
    return reserved;
}

[[nodiscard]] bool mbr_match_data_source(const std::byte* entry, const std::uint32_t sector,
                                         const std::uint64_t source_start) noexcept {
    const auto type = static_cast<std::uint8_t>(entry[4]);
    if (type == 0 || is_mbr_structure_or_system_type(type)) {
        return false;
    }
    const auto first = static_cast<std::uint64_t>(read_le32(entry + 8));
    const auto start = first * static_cast<std::uint64_t>(sector);
    const auto delta = start >= source_start ? start - source_start : source_start - start;
    return delta <= sector;
}

[[nodiscard]] base::Result<void>
validate_mbr_final_layout(const std::vector<std::byte>& mbr, const std::uint32_t sector,
                          const std::uint64_t target_disk_size_bytes,
                          const std::span<const PartitionLayoutEdit> resolved_edits) {
    constexpr std::size_t kTable = 0x1BE;
    constexpr std::size_t kEntry = 16;
    if (mbr.size() < kTable + kEntry * 4 || sector == 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR layout is incomplete for validation"});
    }
    const auto usable_start = mbr_min_data_start_bytes(sector);
    const auto usable_end = mbr_usable_end_bytes(sector, target_disk_size_bytes);
    if (usable_end <= usable_start) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR usable range is empty"});
    }
    std::vector<ByteRange> intervals;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* entry = mbr.data() + kTable + i * kEntry;
        const auto type = static_cast<std::uint8_t>(entry[4]);
        if (type == 0) {
            continue;
        }
        auto first = static_cast<std::uint64_t>(read_le32(entry + 8));
        auto count = static_cast<std::uint64_t>(read_le32(entry + 12));
        if (!is_mbr_structure_or_system_type(type)) {
            std::uint64_t source_start = 0;
            if (!checked_mul_u64(first, sector, source_start)) {
                return base::Result<void>::failure(
                    {base::ErrorCode::kInvalidArgument, "MBR source LBA overflows"});
            }
            for (const auto& edit : resolved_edits) {
                const auto delta = source_start >= edit.source_start_offset_bytes
                                       ? source_start - edit.source_start_offset_bytes
                                       : edit.source_start_offset_bytes - source_start;
                if (delta > sector) {
                    continue;
                }
                first = edit.target_start_offset_bytes / sector;
                count = edit.size_bytes / sector;
                break;
            }
        }
        if (count == 0) {
            continue;
        }
        // No partition may own LBA 0 (the MBR sector).
        if (first == 0) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "MBR partition must not start at LBA 0"});
        }
        auto range = mbr_entry_to_range(first, count, sector);
        if (!range) {
            return base::Result<void>::failure(range.error());
        }
        // System/structure types may live below preferred data start (e.g. early utility).
        // Data intervals must still sit inside usable_start..usable_end.
        if (!is_mbr_structure_or_system_type(type)) {
            if (range.value().start < usable_start || range.value().end > usable_end) {
                return base::Result<void>::failure(
                    {base::ErrorCode::kInvalidArgument,
                     "MBR data partition outside target disk usable range"});
            }
        } else if (range.value().end > usable_end) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument,
                 "MBR system partition exceeds target disk size"});
        }
        intervals.push_back(range.value());
    }
    // Overlap check across all types; usable_start=0 so structure near LBA1 is allowed.
    return validate_final_intervals(std::move(intervals), 0, usable_end);
}

[[nodiscard]] base::Result<PartitionLayoutEdit>
resolve_one_mbr_edit(const MbrEditResolveContext& ctx, const PartitionLayoutEdit& edit) {
    if (ctx.mbr == nullptr || ctx.reserved == nullptr || ctx.sector == 0) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR edit resolve context is incomplete"});
    }
    bool found = false;
    for (std::size_t i = 0; i < 4; ++i) {
        const auto* entry = ctx.mbr->data() + kMbrTableOffset + i * kMbrEntrySize;
        if (mbr_match_data_source(entry, ctx.sector, edit.source_start_offset_bytes)) {
            found = true;
            break;
        }
    }
    if (!found) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kNotFound, "MBR partition layout edit source not found"});
    }
    const auto min_start = mbr_min_data_start_bytes(ctx.sector);
    auto start =
        (edit.target_start_offset_bytes / ctx.sector) * static_cast<std::uint64_t>(ctx.sector);
    auto size = (edit.size_bytes / ctx.sector) * static_cast<std::uint64_t>(ctx.sector);
    if (start < min_start) {
        start = min_start;
    }
    if (start < ctx.cursor) {
        start = ctx.cursor;
    }
    clamp_range_from_reserved(start, size, ctx.usable_end, ctx.sector, *ctx.reserved);
    if (size == 0 || start < min_start) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument,
             "MBR partition layout has no free space after reserved ranges"});
    }
    const auto first_lba = start / ctx.sector;
    const auto count = size / ctx.sector;
    if (!mbr_lba_fields_ok(first_lba, count)) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument,
             "MBR partition LBA or sector count exceeds uint32 range"});
    }
    std::uint64_t placed_end = 0;
    if (!checked_add_u64(start, size, placed_end) || placed_end > ctx.usable_end) {
        return base::Result<PartitionLayoutEdit>::failure(
            {base::ErrorCode::kInvalidArgument, "resolved MBR partition exceeds target disk"});
    }
    return base::Result<PartitionLayoutEdit>::success(
        PartitionLayoutEdit{edit.source_start_offset_bytes, start, size});
}

[[nodiscard]] base::Result<std::vector<PartitionLayoutEdit>>
resolve_mbr_layout_edits(const std::vector<std::byte>& mbr, const std::uint32_t sector_size,
                         const std::uint64_t target_disk_size_bytes,
                         const std::span<const PartitionLayoutEdit> edits) {
    std::vector<PartitionLayoutEdit> out;
    constexpr std::size_t kTable = 0x1BE;
    constexpr std::size_t kEntry = 16;
    if (mbr.size() < kTable + kEntry * 4 || edits.empty()) {
        return base::Result<std::vector<PartitionLayoutEdit>>::success(std::move(out));
    }
    const auto sector = sector_size == 0 ? 512U : sector_size;
    auto inputs = validate_layout_edit_inputs(edits, sector, true);
    if (!inputs) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(inputs.error());
    }
    const auto usable_end = mbr_usable_end_bytes(sector, target_disk_size_bytes);
    if (usable_end <= mbr_min_data_start_bytes(sector)) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(
            {base::ErrorCode::kInvalidArgument, "target disk is too small for MBR data"});
    }
    const auto reserved = collect_mbr_reserved_ranges(mbr, sector);
    std::vector<PartitionLayoutEdit> ordered(edits.begin(), edits.end());
    std::sort(ordered.begin(), ordered.end(),
              [](const PartitionLayoutEdit& a, const PartitionLayoutEdit& b) {
                  return a.source_start_offset_bytes < b.source_start_offset_bytes;
              });
    MbrEditResolveContext ctx{&mbr, sector, usable_end, mbr_min_data_start_bytes(sector),
                              &reserved};
    out.reserve(ordered.size());
    for (const auto& edit : ordered) {
        auto placed = resolve_one_mbr_edit(ctx, edit);
        if (!placed) {
            return base::Result<std::vector<PartitionLayoutEdit>>::failure(placed.error());
        }
        ctx.cursor = placed.value().target_start_offset_bytes + placed.value().size_bytes;
        out.push_back(std::move(placed).value());
    }
    auto final_ok = validate_mbr_final_layout(mbr, sector, target_disk_size_bytes, out);
    if (!final_ok) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(final_ok.error());
    }
    return base::Result<std::vector<PartitionLayoutEdit>>::success(std::move(out));
}

[[nodiscard]] base::Result<void>
apply_one_gpt_edit(std::vector<std::byte>& entries, const std::uint32_t entry_size,
                   const std::uint32_t entry_count, const std::uint32_t sector,
                   const PartitionLayoutEdit& edit) {
    auto target_start =
        (edit.target_start_offset_bytes / sector) * static_cast<std::uint64_t>(sector);
    auto size = (edit.size_bytes / sector) * static_cast<std::uint64_t>(sector);
    if (size == 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout size is zero"});
    }
    const auto sector_count = size / sector;
    const auto new_first = target_start / sector;
    if (sector_count == 0 ||
        new_first > (std::numeric_limits<std::uint64_t>::max)() - (sector_count - 1ULL)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT last LBA overflows"});
    }
    const auto new_last = new_first + sector_count - 1ULL;
    for (std::uint32_t i = 0; i < entry_count; ++i) {
        auto* entry = entries.data() + static_cast<std::size_t>(i) * entry_size;
        if (is_zero_guid_bytes(entry)) {
            continue;
        }
        GUID type{};
        std::memcpy(&type, entry, sizeof(type));
        if (!guid_equal(type, kGptBasicDataPartitionType)) {
            continue;
        }
        const auto first_lba = read_le64(entry + 32);
        const auto start = first_lba * static_cast<std::uint64_t>(sector);
        const auto delta = start >= edit.source_start_offset_bytes
                               ? start - edit.source_start_offset_bytes
                               : edit.source_start_offset_bytes - start;
        if (delta > sector) {
            continue;
        }
        const auto base = static_cast<std::size_t>(i) * entry_size;
        write_le64(entries, base + 32, new_first);
        write_le64(entries, base + 40, new_last);
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(
        {base::ErrorCode::kNotFound, "partition layout edit source start not found"});
}

[[nodiscard]] base::Result<void>
apply_gpt_partition_layout_edits(GptPartitionLayoutApply& ctx) {
    if (ctx.entries == nullptr || ctx.header == nullptr) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT layout pointers are null"});
    }
    auto& entries = *ctx.entries;
    auto& header = *ctx.header;
    if (header.size() < 92 || entries.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT layout is incomplete for apply"});
    }
    const auto entry_size = read_le32(header.data() + 84);
    const auto entry_count = read_le32(header.data() + 80);
    const auto array_bytes =
        static_cast<std::uint64_t>(entry_size) * static_cast<std::uint64_t>(entry_count);
    if (entry_size < 128 || entry_count == 0 || array_bytes > entries.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "GPT partition entry array is invalid"});
    }
    const auto sector = ctx.sector_size == 0 ? 512U : ctx.sector_size;
    if (!ctx.edits.empty()) {
        auto inputs = validate_layout_edit_inputs(ctx.edits, sector, false);
        if (!inputs) {
            return inputs;
        }
    }
    auto final_ok =
        validate_gpt_final_layout(entries, header, sector, ctx.target_disk_size_bytes, ctx.edits);
    if (!final_ok) {
        return final_ok;
    }
    for (const auto& edit : ctx.edits) {
        auto applied = apply_one_gpt_edit(entries, entry_size, entry_count, sector, edit);
        if (!applied) {
            return applied;
        }
    }
    update_gpt_header_geometry_for_target(header, sector, ctx.target_disk_size_bytes, array_bytes,
                                          ctx.role);
    const auto array_span =
        std::span<const std::byte>(entries.data(), static_cast<std::size_t>(array_bytes));
    recompute_gpt_header_crcs(header, array_span);
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
apply_one_mbr_edit(std::vector<std::byte>& mbr, const std::uint32_t sector,
                   const PartitionLayoutEdit& edit) {
    constexpr std::size_t kTable = 0x1BE;
    constexpr std::size_t kEntry = 16;
    const auto first_lba = edit.target_start_offset_bytes / sector;
    const auto count = edit.size_bytes / sector;
    if (!mbr_lba_fields_ok(first_lba, count)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "MBR partition LBA or sector count exceeds uint32 range"});
    }
    for (std::size_t i = 0; i < 4; ++i) {
        auto* entry = mbr.data() + kTable + i * kEntry;
        if (!mbr_match_data_source(entry, sector, edit.source_start_offset_bytes)) {
            continue;
        }
        write_le32(mbr, kTable + i * kEntry + 8, static_cast<std::uint32_t>(first_lba));
        write_le32(mbr, kTable + i * kEntry + 12, static_cast<std::uint32_t>(count));
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(
        {base::ErrorCode::kNotFound, "MBR partition layout edit source not found"});
}

[[nodiscard]] base::Result<void>
apply_mbr_partition_layout_edits(std::vector<std::byte>& mbr, const std::uint32_t sector_size,
                                 const std::uint64_t target_disk_size_bytes,
                                 const std::span<const PartitionLayoutEdit> edits) {
    if (mbr.size() < kMbrTableOffset + kMbrEntrySize * 4) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR layout is incomplete for apply"});
    }
    const auto sector = sector_size == 0 ? 512U : sector_size;
    auto extended = reject_mbr_extended_partitions(mbr);
    if (!extended) {
        return extended;
    }
    if (!edits.empty()) {
        auto inputs = validate_layout_edit_inputs(edits, sector, true);
        if (!inputs) {
            return inputs;
        }
    }
    auto final_ok = validate_mbr_final_layout(mbr, sector, target_disk_size_bytes, edits);
    if (!final_ok) {
        return final_ok;
    }
    for (const auto& edit : edits) {
        auto applied = apply_one_mbr_edit(mbr, sector, edit);
        if (!applied) {
            return applied;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_gpt_raw_layout_for_restore(const WindowsRawDiskLayout& layout, const std::uint32_t sector,
                                    const std::uint64_t target_disk_size_bytes) {
    if (layout.mbr_sector.empty() || layout.gpt_primary_header.empty() ||
        layout.gpt_partition_entries.empty() || layout.gpt_backup_header.empty() ||
        layout.gpt_backup_entries.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT raw layout is incomplete for restore"});
    }
    auto mbr_sig = verify_mbr_boot_signature(layout.mbr_sector);
    if (!mbr_sig) {
        return mbr_sig;
    }
    auto primary = verify_gpt_header_fields(
        layout.gpt_primary_header,
        std::span<const std::byte>(layout.gpt_partition_entries.data(),
                                   layout.gpt_partition_entries.size()),
        sector);
    if (!primary) {
        return primary;
    }
    auto backup = verify_gpt_header_fields(
        layout.gpt_backup_header,
        std::span<const std::byte>(layout.gpt_backup_entries.data(),
                                   layout.gpt_backup_entries.size()),
        sector);
    if (!backup) {
        return backup;
    }
    auto agree =
        verify_gpt_primary_backup_agreement(layout.gpt_primary_header, layout.gpt_backup_header);
    if (!agree) {
        return agree;
    }
    return validate_gpt_final_layout(layout.gpt_partition_entries, layout.gpt_primary_header,
                                     sector, target_disk_size_bytes, {});
}

[[nodiscard]] base::Result<void>
validate_mbr_raw_layout_for_restore(const WindowsRawDiskLayout& layout, const std::uint32_t sector,
                                    const std::uint64_t target_disk_size_bytes) {
    if (layout.mbr_sector.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR raw layout is incomplete for restore"});
    }
    auto mbr_sig = verify_mbr_boot_signature(layout.mbr_sector);
    if (!mbr_sig) {
        return mbr_sig;
    }
    auto extended = reject_mbr_extended_partitions(layout.mbr_sector);
    if (!extended) {
        return extended;
    }
    return validate_mbr_final_layout(layout.mbr_sector, sector, target_disk_size_bytes, {});
}

} // namespace

base::Result<void>
validate_raw_disk_layout_for_restore(const PartitionLayoutRequest& request) {
    if (request.layout == nullptr) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout pointer is null"});
    }
    const auto sector = request.bytes_per_sector == 0 ? 512U : request.bytes_per_sector;
    if (request.partition_style == "RAW") {
        return base::Result<void>::success();
    }
    if (request.partition_style == "GPT") {
        return validate_gpt_raw_layout_for_restore(*request.layout, sector,
                                                   request.target_disk_size_bytes);
    }
    if (request.partition_style == "MBR") {
        return validate_mbr_raw_layout_for_restore(*request.layout, sector,
                                                   request.target_disk_size_bytes);
    }
    return base::Result<void>::failure(
        {base::ErrorCode::kInvalidArgument, "unsupported partition style for restore validation"});
}

base::Result<std::vector<PartitionLayoutEdit>>
resolve_partition_layout_edits(const PartitionLayoutRequest& request) {
    if (request.layout == nullptr) {
        return base::Result<std::vector<PartitionLayoutEdit>>::failure(
            {base::ErrorCode::kInvalidArgument, "partition layout pointer is null"});
    }
    if (request.edits.empty()) {
        return base::Result<std::vector<PartitionLayoutEdit>>::success({});
    }
    const auto& layout = *request.layout;
    if (request.partition_style == "GPT") {
        if (layout.gpt_partition_entries.empty() || layout.gpt_primary_header.empty()) {
            return base::Result<std::vector<PartitionLayoutEdit>>::failure(
                {base::ErrorCode::kInvalidArgument, "GPT layout is incomplete for resolve"});
        }
        return resolve_gpt_layout_edits(layout.gpt_partition_entries, layout.gpt_primary_header,
                                        request.bytes_per_sector, request.target_disk_size_bytes,
                                        request.edits);
    }
    if (request.partition_style == "MBR") {
        if (layout.mbr_sector.empty()) {
            return base::Result<std::vector<PartitionLayoutEdit>>::failure(
                {base::ErrorCode::kInvalidArgument, "MBR layout is incomplete for resolve"});
        }
        return resolve_mbr_layout_edits(layout.mbr_sector, request.bytes_per_sector,
                                        request.target_disk_size_bytes, request.edits);
    }
    return base::Result<std::vector<PartitionLayoutEdit>>::success({});
}

base::Result<WindowsRawDiskLayout>
apply_partition_layout_edits(WindowsRawDiskLayout layout, const PartitionLayoutRequest& request) {
    if (request.partition_style == "GPT") {
        GptPartitionLayoutApply primary{&layout.gpt_partition_entries, &layout.gpt_primary_header,
                                        request.bytes_per_sector, request.target_disk_size_bytes,
                                        request.edits, GptHeaderRole::Primary};
        auto primary_ok = apply_gpt_partition_layout_edits(primary);
        if (!primary_ok) {
            return base::Result<WindowsRawDiskLayout>::failure(primary_ok.error());
        }
        GptPartitionLayoutApply backup{&layout.gpt_backup_entries, &layout.gpt_backup_header,
                                       request.bytes_per_sector, request.target_disk_size_bytes,
                                       request.edits, GptHeaderRole::Backup};
        auto backup_ok = apply_gpt_partition_layout_edits(backup);
        if (!backup_ok) {
            return base::Result<WindowsRawDiskLayout>::failure(backup_ok.error());
        }
    } else if (request.partition_style == "MBR") {
        auto mbr = apply_mbr_partition_layout_edits(layout.mbr_sector, request.bytes_per_sector,
                                                    request.target_disk_size_bytes, request.edits);
        if (!mbr) {
            return base::Result<WindowsRawDiskLayout>::failure(mbr.error());
        }
    }
    return base::Result<WindowsRawDiskLayout>::success(std::move(layout));
}
} // namespace aegra::adapters::windows_disk

