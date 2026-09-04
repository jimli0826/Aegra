#include "aegra/format/manifest.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::format {
namespace {

constexpr std::array<std::byte, 18> kFingerprintDomain{
    std::byte{'A'}, std::byte{'E'}, std::byte{'G'}, std::byte{'R'}, std::byte{'A'}, std::byte{'-'},
    std::byte{'B'}, std::byte{'O'}, std::byte{'O'}, std::byte{'T'}, std::byte{'-'}, std::byte{'D'},
    std::byte{'I'}, std::byte{'S'}, std::byte{'K'}, std::byte{'-'}, std::byte{'V'}, std::byte{'1'},
};

void append_u8(std::vector<std::byte>& output, const std::uint8_t value) {
    output.push_back(static_cast<std::byte>(value));
}

void append_u32(std::vector<std::byte>& output, const std::uint32_t value) {
    for (unsigned shift = 0; shift < 32; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_u64(std::vector<std::byte>& output, const std::uint64_t value) {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xFFU));
    }
}

void append_bytes(std::vector<std::byte>& output, const std::span<const std::byte> value) {
    append_u64(output, static_cast<std::uint64_t>(value.size()));
    output.insert(output.end(), value.begin(), value.end());
}

void append_string(std::vector<std::byte>& output, const std::string_view value) {
    append_u64(output, static_cast<std::uint64_t>(value.size()));
    for (const char character : value) {
        output.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
}

void append_partition(std::vector<std::byte>& output, const Partition& partition) {
    append_u32(output, partition.partition_number);
    append_u64(output, partition.offset);
    append_u64(output, partition.size);
    append_u8(output, static_cast<std::uint8_t>(partition.style));
    append_u8(output, partition.is_active ? 1U : 0U);
    append_u8(output, partition.mbr_type);
    append_string(output, partition.gpt_type_guid);
    append_string(output, partition.gpt_name);
}

} // namespace

base::Result<std::vector<std::byte>>
encode_boot_disk_layout_fingerprint_preimage(const Disk& disk) {
    if (disk.disk_size == 0 || disk.bytes_per_sector == 0 || disk.partitions.empty()) {
        return base::Result<std::vector<std::byte>>::failure({
            base::ErrorCode::kInvalidArgument,
            "boot disk fingerprint requires complete disk geometry",
        });
    }

    std::vector<Partition> partitions = disk.partitions;
    std::ranges::sort(partitions, {}, &Partition::partition_number);

    std::vector<std::byte> output;
    output.reserve(256U + disk.raw_layout.mbr_sector.size() +
                   disk.raw_layout.gpt_primary_header.size() +
                   disk.raw_layout.gpt_partition_entries.size() +
                   disk.raw_layout.gpt_backup_header.size() +
                   disk.raw_layout.gpt_backup_entries.size());
    output.insert(output.end(), kFingerprintDomain.begin(), kFingerprintDomain.end());
    append_u64(output, disk.disk_size);
    append_u32(output, disk.bytes_per_sector);
    append_u64(output, disk.total_sectors);
    append_u8(output, static_cast<std::uint8_t>(disk.partition_style));
    append_u64(output, static_cast<std::uint64_t>(partitions.size()));
    for (const auto& partition : partitions) {
        append_partition(output, partition);
    }
    append_bytes(output, disk.raw_layout.mbr_sector);
    append_bytes(output, disk.raw_layout.gpt_primary_header);
    append_bytes(output, disk.raw_layout.gpt_partition_entries);
    append_bytes(output, disk.raw_layout.gpt_backup_header);
    append_bytes(output, disk.raw_layout.gpt_backup_entries);
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

bool compatible_boot_profiles(const std::optional<BootProfile>& left,
                              const std::optional<BootProfile>& right) noexcept {
    if (left.has_value() != right.has_value()) {
        return false;
    }
    if (!left) {
        return true;
    }
    return left->profile_version == right->profile_version &&
           left->system_disk_number == right->system_disk_number &&
           left->windows_volume_index == right->windows_volume_index &&
           left->required_boot_partition_numbers == right->required_boot_partition_numbers &&
           left->firmware_mode == right->firmware_mode &&
           left->os_architecture == right->os_architecture &&
           left->logical_sector_size == right->logical_sector_size &&
           left->layout_fingerprint_algorithm == right->layout_fingerprint_algorithm &&
           left->layout_fingerprint == right->layout_fingerprint &&
           left->probe_protocol_version == right->probe_protocol_version;
}

} // namespace aegra::format
