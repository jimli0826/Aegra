#include "partition_filter.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace aegra::adapters::dokan::detail {
namespace {

std::string to_lower_ascii(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

bool is_reserved_gpt_type_guid(const std::string& type_guid) {
    const std::string g = to_lower_ascii(type_guid);
    if (g.empty()) {
        return false;
    }
    static constexpr const char* kReserved[] = {
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", // EFI
        "e3c9e316-0b5c-4db8-817d-f92df00215ae", // MSR
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", // WinRE
        "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", // LDM metadata
        "af9b60a0-1431-4f62-bc68-3311714a69ad", // LDM data
        "e75caf8f-f680-4cee-afa3-b001e56efc2d", // Storage Spaces
        "558d43c5-a1ac-43c0-aac8-d1472b2923d1",
        "37affc90-ef7d-4e96-91c3-2d7ae055b174",
        "00000000-0000-0000-0000-000000000000",
    };
    for (const char* reserved : kReserved) {
        if (g == reserved) {
            return true;
        }
    }
    return false;
}

bool is_reserved_mbr_type(std::uint8_t mbr_type) {
    switch (mbr_type) {
    case 0x00:
    case 0x05:
    case 0x0F:
    case 0x27:
    case 0xEE:
    case 0xEF:
        return true;
    default:
        return false;
    }
}

bool name_looks_reserved(const std::string& name) {
    const std::string n = to_lower_ascii(name);
    if (n.empty()) {
        return false;
    }
    static constexpr const char* kKeys[] = {
        "efi",      "system",   "msr",      "reserved", "recovery",
        "winre",    "oem",      "diag",     "diagnostics", "restore",
        "lenovo",   "dell",     "hp_",      "recovery partition",
        "microsoft reserved",
    };
    for (const char* key : kKeys) {
        if (n.find(key) != std::string::npos) {
            return true;
        }
    }
    return false;
}

bool filesystem_looks_mountable(const std::string& fs_name) {
    const std::string f = to_lower_ascii(fs_name);
    if (f.empty()) {
        return false;
    }
    return f == "ntfs" || f == "fat32" || f == "fat" || f == "exfat" || f == "refs" ||
           f == "udf" || f.find("fat") != std::string::npos;
}

const format::Disk* find_disk(const format::Manifest& manifest,
                              std::uint32_t disk_number) {
    for (const auto& disk : manifest.disks) {
        if (disk.disk_number == disk_number) {
            return &disk;
        }
    }
    return nullptr;
}

bool partition_ok_on_disk(const format::Disk* disk, std::uint32_t partition_number) {
    if (disk == nullptr) {
        return true;
    }
    for (const auto& part : disk->partitions) {
        if (part.partition_number == partition_number) {
            return is_mountable_partition(part);
        }
    }
    return true;
}

} // namespace

bool is_mountable_partition(const format::Partition& part) {
    if (filesystem_looks_mountable(part.filesystem) &&
        !name_looks_reserved(part.volume_label) &&
        !name_looks_reserved(part.gpt_name)) {
        if (part.style == format::PartitionStyle::kGpt &&
            is_reserved_gpt_type_guid(part.gpt_type_guid)) {
            return false;
        }
        if (part.style == format::PartitionStyle::kMbr &&
            is_reserved_mbr_type(part.mbr_type)) {
            return false;
        }
        return true;
    }

    if (part.style == format::PartitionStyle::kGpt) {
        if (is_reserved_gpt_type_guid(part.gpt_type_guid)) {
            return false;
        }
        if (name_looks_reserved(part.gpt_name)) {
            return false;
        }
    } else if (part.style == format::PartitionStyle::kMbr) {
        if (is_reserved_mbr_type(part.mbr_type)) {
            return false;
        }
    }

    if (name_looks_reserved(part.volume_label) || name_looks_reserved(part.gpt_name)) {
        return false;
    }

    if (filesystem_looks_mountable(part.filesystem)) {
        return true;
    }

    return part.size >= 256ULL * 1024 * 1024;
}

std::set<std::uint32_t>
find_data_partition_numbers(const format::Manifest& manifest,
                            std::uint32_t source_disk_number) {
    std::set<std::uint32_t> result;
    const format::Disk* disk = find_disk(manifest, source_disk_number);

    for (const auto& volume : manifest.volumes) {
        if (!volume.filesystem.empty() &&
            !filesystem_looks_mountable(volume.filesystem)) {
            continue;
        }
        if (volume.total_size > 0 && volume.total_size < 256ULL * 1024 * 1024) {
            continue;
        }
        for (const auto& extent : volume.extents) {
            if (extent.disk_number != source_disk_number) {
                continue;
            }
            if (extent.partition_number == 0) {
                continue;
            }
            if (!partition_ok_on_disk(disk, extent.partition_number)) {
                continue;
            }
            result.insert(extent.partition_number);
        }
    }

    if (disk != nullptr) {
        for (const auto& part : disk->partitions) {
            if (is_mountable_partition(part)) {
                result.insert(part.partition_number);
            }
        }
    }
    return result;
}

bool manifest_has_disk(const format::Manifest& manifest,
                       std::uint32_t source_disk_number) {
    return find_disk(manifest, source_disk_number) != nullptr;
}

} // namespace aegra::adapters::dokan::detail
