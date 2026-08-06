#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <bcrypt.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace aegra::adapters::windows_disk {
namespace {

[[nodiscard]] std::wstring physical_drive_path(const std::uint32_t disk_number) {
    return std::wstring(LR"(\\.\PhysicalDrive)") + std::to_wstring(disk_number);
}

[[nodiscard]] detail::UniqueHandle open_disk_read_write(const std::uint32_t disk_number) {
    return detail::UniqueHandle(CreateFileW(physical_drive_path(disk_number).c_str(),
                                            GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
}

void update_disk_properties(const HANDLE handle) noexcept {
    DWORD returned = 0;
    DeviceIoControl(handle, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
}

[[nodiscard]] base::Result<std::uint64_t> disk_length_bytes(const HANDLE handle) {
    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                         &returned, nullptr) ||
        returned < sizeof(length) || length.Length.QuadPart <= 0) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kIoFailure, "target disk length is unavailable"});
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(length.Length.QuadPart));
}

[[nodiscard]] base::Result<void> fill_random(const std::span<std::byte> destination) {
    if (destination.empty()) {
        return base::Result<void>::success();
    }
    const auto status = BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(destination.data()),
                                        static_cast<ULONG>(destination.size()),
                                        BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    if (status != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInternal, "failed to generate disk signature bytes"});
    }
    return base::Result<void>::success();
}

// IEEE CRC32 used by GPT headers (polynomial 0xEDB88320).
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

[[nodiscard]] base::Result<void> resignature_mbr(std::vector<std::byte>& mbr_sector) {
    // Disk signature at offset 0x1B8 (4 bytes).
    if (mbr_sector.size() < 0x1BC) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "MBR sector is too small to resignature"});
    }
    return fill_random(std::span<std::byte>(mbr_sector.data() + 0x1B8, 4));
}

[[nodiscard]] base::Result<void>
resignature_gpt_header(std::vector<std::byte>& header, const std::array<std::byte, 16>& disk_guid) {
    // GPT header: Disk GUID at offset 56 (16 bytes); Header CRC32 at offset 16.
    if (header.size() < 92) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT header is too small to resignature"});
    }
    std::memcpy(header.data() + 56, disk_guid.data(), disk_guid.size());
    write_le32(header, 16, 0);
    std::uint32_t header_size = 0;
    std::memcpy(&header_size, header.data() + 12, sizeof(header_size));
    if (header_size < 92 || header_size > header.size()) {
        header_size = 92;
    }
    const auto crc =
        crc32_ieee(std::span<const std::byte>(header.data(), static_cast<std::size_t>(header_size)));
    write_le32(header, 16, crc);
    return base::Result<void>::success();
}

// Microsoft basic data partition type GUID (on-disk mixed-endian layout).
constexpr GUID kGptBasicDataPartitionType{0xEBD0A0A2,
                                          0xB9E5,
                                          0x4433,
                                          {0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7}};

[[nodiscard]] bool guid_equal(const GUID& left, const GUID& right) noexcept {
    return std::memcmp(&left, &right, sizeof(GUID)) == 0;
}

// Align with old PartitionManager::IsGptBasicDataPartition — only basic data is expanded.
[[nodiscard]] bool is_gpt_basic_data_partition(const PARTITION_INFORMATION_GPT& gpt) noexcept {
    return guid_equal(gpt.PartitionType, kGptBasicDataPartitionType);
}

[[nodiscard]] bool is_reserved_mbr_partition(const std::uint8_t type) noexcept {
    switch (type) {
    case 0x00: // unused
    case 0x05: // extended
    case 0x0F: // extended LBA
    case 0x12:
    case 0x17:
    case 0x1B:
    case 0x1C:
    case 0x27:
    case 0xDE:
    case 0xEE: // GPT protective
    case 0xEF: // EFI system
        return true;
    default:
        return false;
    }
}

[[nodiscard]] base::Result<std::vector<std::byte>> read_drive_layout(const HANDLE handle) {
    std::vector<std::byte> buffer(sizeof(DRIVE_LAYOUT_INFORMATION_EX) +
                                  sizeof(PARTITION_INFORMATION_EX) * 128U);
    for (int attempt = 0; attempt < 4; ++attempt) {
        DWORD returned = 0;
        if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, buffer.data(),
                            static_cast<DWORD>(buffer.size()), &returned, nullptr)) {
            if (returned < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
                return base::Result<std::vector<std::byte>>::failure(
                    {base::ErrorCode::kIoFailure, "drive layout response is truncated"});
            }
            buffer.resize(returned);
            return base::Result<std::vector<std::byte>>::success(std::move(buffer));
        }
        const auto error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER) {
            return base::Result<std::vector<std::byte>>::failure(
                detail::win32_error(error, "IOCTL_DISK_GET_DRIVE_LAYOUT_EX"));
        }
        buffer.resize(buffer.size() * 2U);
    }
    return base::Result<std::vector<std::byte>>::failure(
        {base::ErrorCode::kIoFailure, "drive layout buffer is too small"});
}

struct ExpandCandidate final {
    std::uint32_t partition_number{0};
    std::uint64_t starting_offset{0};
    std::uint64_t partition_length{0};
    std::uint64_t free_after_bytes{0};
    bool is_gpt{false};
};

// Old PartitionManager::ExtendLastDataPartitionOnDisk candidate selection.
[[nodiscard]] std::optional<ExpandCandidate>
find_last_data_partition(const DRIVE_LAYOUT_INFORMATION_EX& layout,
                         const std::uint64_t disk_size_bytes) {
    std::optional<ExpandCandidate> best;
    for (DWORD index = 0; index < layout.PartitionCount; ++index) {
        const auto& part = layout.PartitionEntry[index];
        if (part.PartitionLength.QuadPart <= 0 || part.PartitionNumber == 0) {
            continue;
        }
        bool is_data = false;
        if (part.PartitionStyle == PARTITION_STYLE_GPT) {
            is_data = is_gpt_basic_data_partition(part.Gpt);
        } else if (part.PartitionStyle == PARTITION_STYLE_MBR) {
            is_data = !is_reserved_mbr_partition(part.Mbr.PartitionType) &&
                      part.Mbr.RecognizedPartition != FALSE;
        }
        if (!is_data) {
            continue;
        }
        ExpandCandidate candidate;
        candidate.partition_number = part.PartitionNumber;
        candidate.starting_offset = static_cast<std::uint64_t>(part.StartingOffset.QuadPart);
        candidate.partition_length = static_cast<std::uint64_t>(part.PartitionLength.QuadPart);
        candidate.is_gpt = part.PartitionStyle == PARTITION_STYLE_GPT;
        if (!best || candidate.starting_offset >= best->starting_offset) {
            best = candidate;
        }
    }
    if (!best) {
        return std::nullopt;
    }
    auto next_start = disk_size_bytes;
    for (DWORD index = 0; index < layout.PartitionCount; ++index) {
        const auto& part = layout.PartitionEntry[index];
        if (part.PartitionLength.QuadPart <= 0) {
            continue;
        }
        const auto offset = static_cast<std::uint64_t>(part.StartingOffset.QuadPart);
        if (offset > best->starting_offset && offset < next_start) {
            next_start = offset;
        }
    }
    const auto end = best->starting_offset + best->partition_length;
    best->free_after_bytes = next_start > end ? next_start - end : 0;
    return best;
}

// Open path without trailing backslash (CreateFileW volume GUID form).
[[nodiscard]] std::wstring trim_volume_open_path(std::wstring path) {
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

// Retry while mount manager settles after layout rebuild (old AutoExtend: up to 10 × 1s).
[[nodiscard]] base::Result<std::wstring>
find_volume_open_path_for_partition(const std::uint32_t disk_number,
                                    const std::uint64_t offset_bytes) {
    for (int attempt = 1; attempt <= 10; ++attempt) {
        std::array<wchar_t, MAX_PATH> volume_name{};
        const auto find =
            FindFirstVolumeW(volume_name.data(), static_cast<DWORD>(volume_name.size()));
        if (find == INVALID_HANDLE_VALUE) {
            return base::Result<std::wstring>::failure(
                detail::win32_error(GetLastError(), "FindFirstVolumeW"));
        }
        std::wstring match;
        do {
            const auto open_path = trim_volume_open_path(volume_name.data());
            detail::UniqueHandle volume(CreateFileW(
                open_path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                nullptr, OPEN_EXISTING, 0, nullptr));
            if (!volume.valid()) {
                continue;
            }
            std::vector<std::byte> extent_buffer(sizeof(VOLUME_DISK_EXTENTS) +
                                                 sizeof(DISK_EXTENT) * 16U);
            DWORD returned = 0;
            if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                                 extent_buffer.data(), static_cast<DWORD>(extent_buffer.size()),
                                 &returned, nullptr) ||
                returned < sizeof(VOLUME_DISK_EXTENTS)) {
                continue;
            }
            const auto* extents =
                reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extent_buffer.data());
            for (DWORD index = 0; index < extents->NumberOfDiskExtents; ++index) {
                const auto& extent = extents->Extents[index];
                if (extent.DiskNumber == disk_number &&
                    static_cast<std::uint64_t>(extent.StartingOffset.QuadPart) == offset_bytes) {
                    match = open_path;
                    break;
                }
            }
            if (!match.empty()) {
                break;
            }
        } while (FindNextVolumeW(find, volume_name.data(), static_cast<DWORD>(volume_name.size())));
        FindVolumeClose(find);
        if (!match.empty()) {
            return base::Result<std::wstring>::success(std::move(match));
        }
        if (attempt < 10) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
    return base::Result<std::wstring>::failure(
        {base::ErrorCode::kNotFound, "restored volume path was not found for expand"});
}

// FAT cannot online-extend; NTFS/ReFS can. Unknown → try extend (old project behavior).
enum class FilesystemExtendKind : std::uint8_t {
    kSkipFat = 1,
    kTryExtend = 2,
};

[[nodiscard]] FilesystemExtendKind filesystem_extend_kind(const std::wstring& open_path) {
    std::wstring root = open_path;
    if (root.empty() || root.back() != L'\\') {
        root.push_back(L'\\');
    }
    std::array<wchar_t, MAX_PATH> fs_name{};
    if (!GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name.data(),
                               static_cast<DWORD>(fs_name.size()))) {
        return FilesystemExtendKind::kTryExtend;
    }
    std::wstring name(fs_name.data());
    for (auto& ch : name) {
        ch = static_cast<wchar_t>(towupper(ch));
    }
    if (name == L"FAT" || name == L"FAT12" || name == L"FAT16" || name == L"FAT32" ||
        name == L"EXFAT") {
        return FilesystemExtendKind::kSkipFat;
    }
    return FilesystemExtendKind::kTryExtend;
}

// Old PartitionManager::ExtendVolumeByPath: keep volume handle open across grow + FSCTL_EXTEND.
[[nodiscard]] base::Result<void>
extend_volume_by_open_path(const std::wstring& open_path, const std::uint32_t disk_number,
                           const ExpandCandidate& candidate) {
    detail::UniqueHandle volume(CreateFileW(open_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW volume for extend"));
    }

    std::vector<std::byte> extent_buffer(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 16U);
    DWORD returned = 0;
    if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                         extent_buffer.data(), static_cast<DWORD>(extent_buffer.size()), &returned,
                         nullptr) ||
        returned < sizeof(VOLUME_DISK_EXTENTS)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS"));
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(extent_buffer.data());
    if (extents->NumberOfDiskExtents == 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "volume has no disk extents"});
    }
    const auto extent_length =
        static_cast<std::uint64_t>(extents->Extents[0].ExtentLength.QuadPart);
    const auto start_offset =
        static_cast<std::uint64_t>(extents->Extents[0].StartingOffset.QuadPart);
    if (extents->Extents[0].DiskNumber != disk_number ||
        start_offset != candidate.starting_offset) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "volume extents do not match expand candidate"});
    }

    auto disk = open_disk_read_write(disk_number);
    if (!disk.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW disk for extend"));
    }

    DISK_GEOMETRY geometry{};
    if (!DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &geometry,
                         sizeof(geometry), &returned, nullptr) ||
        geometry.BytesPerSector == 0) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GET_DRIVE_GEOMETRY"));
    }
    const auto sector = static_cast<std::uint64_t>(geometry.BytesPerSector);

    auto disk_size = disk_length_bytes(disk.get());
    if (!disk_size) {
        return base::Result<void>::failure(disk_size.error());
    }

    auto layout_buffer = read_drive_layout(disk.get());
    if (!layout_buffer) {
        return base::Result<void>::failure(layout_buffer.error());
    }
    const auto* layout =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layout_buffer.value().data());
    auto next_start = disk_size.value();
    std::uint32_t partition_number = 0;
    bool found = false;
    for (DWORD index = 0; index < layout->PartitionCount; ++index) {
        const auto& part = layout->PartitionEntry[index];
        const auto offset = static_cast<std::uint64_t>(part.StartingOffset.QuadPart);
        if (offset == start_offset) {
            partition_number = part.PartitionNumber;
            found = true;
        } else if (part.PartitionLength.QuadPart > 0 && offset > start_offset &&
                   offset < next_start) {
            next_start = offset;
        }
    }
    if (!found || partition_number == 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kNotFound, "partition for volume was not found in drive layout"});
    }

    const auto current_end = start_offset + extent_length;
    if (next_start <= current_end) {
        return base::Result<void>::success();
    }
    auto available = next_start - current_end;
    // Old project: reserve 1 MiB at end of GPT disks for backup header safety.
    if (next_start == disk_size.value() &&
        (candidate.is_gpt || layout->PartitionStyle == PARTITION_STYLE_GPT)) {
        constexpr std::uint64_t kGptReserveBytes = 1024ULL * 1024ULL;
        if (available > kGptReserveBytes) {
            available -= kGptReserveBytes;
        }
    }
    available = (available / sector) * sector;
    if (available == 0) {
        return base::Result<void>::success();
    }

    DISK_GROW_PARTITION grow{};
    grow.PartitionNumber = partition_number;
    grow.BytesToGrow.QuadPart = static_cast<LONGLONG>(available);
    if (!DeviceIoControl(disk.get(), IOCTL_DISK_GROW_PARTITION, &grow, sizeof(grow), nullptr, 0,
                         &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GROW_PARTITION"));
    }
    update_disk_properties(disk.get());

    // FSCTL_EXTEND_VOLUME: NEW total size in sectors (extent before grow + grown bytes).
    // Must use the same open volume handle as the old PartitionManager path.
    const auto new_total_bytes = extent_length + available;
    LONGLONG new_total_sectors = static_cast<LONGLONG>(new_total_bytes / sector);
    if (!DeviceIoControl(volume.get(), FSCTL_EXTEND_VOLUME, &new_total_sectors,
                         sizeof(new_total_sectors), nullptr, 0, &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "FSCTL_EXTEND_VOLUME"));
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<WindowsRawDiskLayout>
apply_disk_signature_policy(WindowsRawDiskLayout layout, const bool preserve_disk_signature,
                            const std::string& partition_style) {
    if (preserve_disk_signature) {
        return base::Result<WindowsRawDiskLayout>::success(std::move(layout));
    }
    if (!layout.mbr_sector.empty()) {
        auto mbr = resignature_mbr(layout.mbr_sector);
        if (!mbr) {
            return base::Result<WindowsRawDiskLayout>::failure(mbr.error());
        }
    }
    if (partition_style == "GPT") {
        std::array<std::byte, 16> disk_guid{};
        auto random = fill_random(disk_guid);
        if (!random) {
            return base::Result<WindowsRawDiskLayout>::failure(random.error());
        }
        if (!layout.gpt_primary_header.empty()) {
            auto primary = resignature_gpt_header(layout.gpt_primary_header, disk_guid);
            if (!primary) {
                return base::Result<WindowsRawDiskLayout>::failure(primary.error());
            }
        }
        if (!layout.gpt_backup_header.empty()) {
            auto backup = resignature_gpt_header(layout.gpt_backup_header, disk_guid);
            if (!backup) {
                return base::Result<WindowsRawDiskLayout>::failure(backup.error());
            }
        }
    }
    return base::Result<WindowsRawDiskLayout>::success(std::move(layout));
}

base::Result<void>
expand_last_data_partition_on_disk(const std::uint32_t disk_number,
                                   const std::uint64_t source_disk_size_bytes,
                                   const std::uint32_t /*bytes_per_sector*/,
                                   const std::string& /*partition_style*/) {
    auto disk = open_disk_read_write(disk_number);
    if (!disk.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk for expand"));
    }
    auto length = disk_length_bytes(disk.get());
    if (!length) {
        return base::Result<void>::failure(length.error());
    }
    if (length.value() <= source_disk_size_bytes) {
        return base::Result<void>::success();
    }
    auto layout_buffer = read_drive_layout(disk.get());
    if (!layout_buffer) {
        return base::Result<void>::failure(layout_buffer.error());
    }
    const auto* layout =
        reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(layout_buffer.value().data());
    auto candidate = find_last_data_partition(*layout, length.value());
    disk.reset();
    if (!candidate) {
        return base::Result<void>::success();
    }
    // Old AutoExtend: ignore free tails smaller than 1 MiB.
    if (candidate->free_after_bytes < 1024ULL * 1024ULL) {
        return base::Result<void>::success();
    }

    auto volume_path =
        find_volume_open_path_for_partition(disk_number, candidate->starting_offset);
    if (!volume_path) {
        return base::Result<void>::failure(volume_path.error());
    }
    if (filesystem_extend_kind(volume_path.value()) == FilesystemExtendKind::kSkipFat) {
        // Leave free space unallocated (FAT/exFAT cannot online-extend).
        return base::Result<void>::success();
    }
    return extend_volume_by_open_path(volume_path.value(), disk_number, *candidate);
}

} // namespace aegra::adapters::windows_disk
