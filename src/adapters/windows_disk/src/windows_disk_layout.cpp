#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <winioctl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <limits>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

[[nodiscard]] detail::UniqueHandle open_physical_drive(const std::uint32_t disk_number) {
    const auto path = std::wstring(LR"(\\.\PhysicalDrive)") + std::to_wstring(disk_number);
    // Prefer GENERIC_READ so MBR/GPT raw sector capture can use ReadFile. Access 0 allows
    // IOCTL metadata only and makes ReadFile fail with ERROR_ACCESS_DENIED.
    constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
    detail::UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ, kShare, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
    if (handle.valid()) {
        return handle;
    }
    // Query-only fallback when exclusive openers deny GENERIC_READ.
    return detail::UniqueHandle(
        CreateFileW(path.c_str(), 0, kShare, nullptr, OPEN_EXISTING, 0, nullptr));
}

[[nodiscard]] std::string partition_style_name(const DWORD style) {
    switch (style) {
    case PARTITION_STYLE_MBR:
        return "MBR";
    case PARTITION_STYLE_GPT:
        return "GPT";
    case PARTITION_STYLE_RAW:
        return "RAW";
    default:
        return "RAW";
    }
}

[[nodiscard]] std::string guid_to_string(const GUID& guid) {
    std::ostringstream stream;
    stream << std::hex << std::setfill('0') << std::nouppercase;
    stream << std::setw(8) << guid.Data1 << '-' << std::setw(4) << guid.Data2 << '-' << std::setw(4)
           << guid.Data3 << '-';
    for (int index = 0; index < 2; ++index) {
        stream << std::setw(2) << static_cast<unsigned>(guid.Data4[index]);
    }
    stream << '-';
    for (int index = 2; index < 8; ++index) {
        stream << std::setw(2) << static_cast<unsigned>(guid.Data4[index]);
    }
    return stream.str();
}

[[nodiscard]] std::string utf16_to_utf8(const std::wstring_view text) {
    if (text.empty()) {
        return {};
    }
    const auto required =
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr);
    if (required <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(),
                            required, nullptr, nullptr) <= 0) {
        return {};
    }
    return result;
}

void fill_disk_size(const HANDLE handle, WindowsPhysicalDiskLayout& layout) {
    GET_LENGTH_INFORMATION length{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                        &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(length) && length.Length.QuadPart > 0) {
        layout.disk_size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    }

    DISK_GEOMETRY_EX geometry{};
    if (DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY_EX, nullptr, 0, &geometry,
                        sizeof(geometry), &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(geometry)) {
        if (layout.disk_size_bytes == 0 && geometry.DiskSize.QuadPart > 0) {
            layout.disk_size_bytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
        }
        if (geometry.Geometry.BytesPerSector > 0) {
            layout.bytes_per_sector = geometry.Geometry.BytesPerSector;
        }
    }
    if (layout.bytes_per_sector == 0) {
        layout.bytes_per_sector = 512;
    }
    if (layout.disk_size_bytes > 0) {
        layout.total_sectors = layout.disk_size_bytes / layout.bytes_per_sector;
    }
}

void fill_storage_identity(const HANDLE handle, WindowsPhysicalDiskLayout& layout) {
    STORAGE_PROPERTY_QUERY query{};
    query.QueryType = PropertyStandardQuery;
    query.PropertyId = StorageDeviceProperty;
    std::array<std::byte, 1024> property_buffer{};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query),
                         property_buffer.data(), static_cast<DWORD>(property_buffer.size()),
                         &bytes_returned, nullptr) ||
        bytes_returned < sizeof(STORAGE_DEVICE_DESCRIPTOR)) {
        layout.media_type = "Unknown";
        return;
    }
    STORAGE_DEVICE_DESCRIPTOR descriptor{};
    std::memcpy(&descriptor, property_buffer.data(), sizeof(descriptor));
    const auto* base = reinterpret_cast<const char*>(property_buffer.data());
    if (descriptor.VendorIdOffset > 0 && descriptor.VendorIdOffset < bytes_returned) {
        layout.model = base + descriptor.VendorIdOffset;
    }
    if (descriptor.ProductIdOffset > 0 && descriptor.ProductIdOffset < bytes_returned) {
        if (!layout.model.empty()) {
            layout.model.push_back(' ');
        }
        layout.model += base + descriptor.ProductIdOffset;
    }
    if (descriptor.SerialNumberOffset > 0 && descriptor.SerialNumberOffset < bytes_returned) {
        layout.serial = base + descriptor.SerialNumberOffset;
    }
    switch (descriptor.BusType) {
    case BusTypeNvme:
        layout.media_type = "SSD";
        break;
    case BusTypeSata:
    case BusTypeAta:
        layout.media_type = "HDD";
        break;
    case BusTypeUsb:
        layout.media_type = "USB";
        break;
    case BusTypeVirtual:
    case BusTypeFileBackedVirtual:
        layout.media_type = "Virtual";
        break;
    default:
        layout.media_type = "Unknown";
        break;
    }
    query.PropertyId = StorageDeviceSeekPenaltyProperty;
    DEVICE_SEEK_PENALTY_DESCRIPTOR seek_penalty{};
    if (DeviceIoControl(handle, IOCTL_STORAGE_QUERY_PROPERTY, &query, sizeof(query), &seek_penalty,
                        sizeof(seek_penalty), &bytes_returned, nullptr) &&
        bytes_returned >= sizeof(seek_penalty) && !seek_penalty.IncursSeekPenalty) {
        layout.media_type = "SSD";
    }
}

[[nodiscard]] WindowsPartitionLayout
partition_from_info(const PARTITION_INFORMATION_EX& entry, const DWORD disk_style) {
    WindowsPartitionLayout partition;
    partition.partition_number = entry.PartitionNumber;
    if (entry.StartingOffset.QuadPart >= 0) {
        partition.offset_bytes = static_cast<std::uint64_t>(entry.StartingOffset.QuadPart);
    }
    if (entry.PartitionLength.QuadPart >= 0) {
        partition.size_bytes = static_cast<std::uint64_t>(entry.PartitionLength.QuadPart);
    }
    partition.partition_style = partition_style_name(entry.PartitionStyle);
    if (entry.PartitionStyle == PARTITION_STYLE_MBR) {
        partition.mbr_type = entry.Mbr.PartitionType;
        partition.is_active = entry.Mbr.BootIndicator != FALSE;
    } else if (entry.PartitionStyle == PARTITION_STYLE_GPT) {
        partition.gpt_type_guid = guid_to_string(entry.Gpt.PartitionType);
        partition.gpt_name = utf16_to_utf8(entry.Gpt.Name);
    } else if (disk_style == PARTITION_STYLE_MBR) {
        partition.partition_style = "MBR";
    } else if (disk_style == PARTITION_STYLE_GPT) {
        partition.partition_style = "GPT";
    }
    return partition;
}

[[nodiscard]] base::Result<void>
read_exact_at(const HANDLE handle, const std::uint64_t offset, std::span<std::byte> destination) {
    if (destination.empty()) {
        return base::Result<void>::success();
    }
    if (destination.size() > (std::numeric_limits<DWORD>::max)()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "raw layout read size is too large"});
    }
    LARGE_INTEGER move{};
    move.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, move, nullptr, FILE_BEGIN)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "raw layout seek failed"});
    }
    DWORD bytes_read = 0;
    if (!ReadFile(handle, destination.data(), static_cast<DWORD>(destination.size()), &bytes_read,
                  nullptr) ||
        bytes_read != destination.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "raw layout read failed"});
    }
    return base::Result<void>::success();
}

// Best-effort MBR/GPT sector capture. Volume backup must not fail when PhysicalDrive
// raw reads are denied; disk→disk restore later rejects archives without raw_layout.
[[nodiscard]] base::Result<void> capture_raw_layout(const HANDLE handle,
                                                    WindowsPhysicalDiskLayout& layout) {
    const auto sector = layout.bytes_per_sector == 0 ? 512U : layout.bytes_per_sector;
    layout.raw_layout = {};
    layout.raw_layout.mbr_sector.assign(sector, std::byte{0});
    auto mbr = read_exact_at(handle, 0, layout.raw_layout.mbr_sector);
    if (!mbr) {
        layout.raw_layout = {};
        return base::Result<void>::success();
    }
    if (layout.partition_style != "GPT") {
        return base::Result<void>::success();
    }
    layout.raw_layout.gpt_primary_header.assign(sector, std::byte{0});
    auto primary = read_exact_at(handle, sector, layout.raw_layout.gpt_primary_header);
    if (!primary) {
        layout.raw_layout.gpt_primary_header.clear();
        return base::Result<void>::success();
    }
    // Standard GPT: 128 entries * 128 bytes starting at LBA 2.
    constexpr std::size_t kGptEntryBytes = 128U * 128U;
    layout.raw_layout.gpt_partition_entries.assign(kGptEntryBytes, std::byte{0});
    auto entries = read_exact_at(handle, static_cast<std::uint64_t>(sector) * 2U,
                                 layout.raw_layout.gpt_partition_entries);
    if (!entries) {
        layout.raw_layout.gpt_partition_entries.clear();
        return base::Result<void>::success();
    }
    if (layout.disk_size_bytes < sector) {
        return base::Result<void>::success();
    }
    const auto backup_header_offset = layout.disk_size_bytes - sector;
    layout.raw_layout.gpt_backup_header.assign(sector, std::byte{0});
    auto backup_header =
        read_exact_at(handle, backup_header_offset, layout.raw_layout.gpt_backup_header);
    if (!backup_header) {
        layout.raw_layout.gpt_backup_header.clear();
        return base::Result<void>::success();
    }
    if (backup_header_offset < kGptEntryBytes) {
        return base::Result<void>::success();
    }
    auto backup_entries_offset = backup_header_offset - kGptEntryBytes;
    backup_entries_offset = (backup_entries_offset / sector) * sector;
    layout.raw_layout.gpt_backup_entries.assign(kGptEntryBytes, std::byte{0});
    auto backup_entries =
        read_exact_at(handle, backup_entries_offset, layout.raw_layout.gpt_backup_entries);
    if (!backup_entries) {
        layout.raw_layout.gpt_backup_entries.clear();
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> fill_partitions(const HANDLE handle,
                                                 WindowsPhysicalDiskLayout& layout) {
    // DRIVE_LAYOUT_INFORMATION_EX embeds PartitionEntry[1]; sizeof includes one entry.
    // Variable entries start at offsetof(..., PartitionEntry). Using sizeof as the base
    // overstates required bytes by one entry and mis-indexes partitions.
    constexpr DWORD kMaxPartitions = 128;
    constexpr auto kPartitionEntriesOffset =
        offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry);
    constexpr auto kHeaderMinBytes = kPartitionEntriesOffset;
    const auto layout_bytes =
        kPartitionEntriesOffset + sizeof(PARTITION_INFORMATION_EX) * kMaxPartitions;
    std::vector<std::byte> layout_buffer(layout_bytes);
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, layout_buffer.data(),
                         static_cast<DWORD>(layout_buffer.size()), &bytes_returned, nullptr) ||
        bytes_returned < kHeaderMinBytes) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk partition layout is unavailable"});
    }
    DRIVE_LAYOUT_INFORMATION_EX header{};
    std::memcpy(&header, layout_buffer.data(), kHeaderMinBytes);
    layout.partition_style = partition_style_name(header.PartitionStyle);
    if (header.PartitionCount > kMaxPartitions) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk reports too many partitions"});
    }
    const auto required_bytes =
        kPartitionEntriesOffset +
        static_cast<std::size_t>(header.PartitionCount) * sizeof(PARTITION_INFORMATION_EX);
    if (required_bytes > bytes_returned || required_bytes > layout_buffer.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk partition layout is truncated"});
    }
    layout.partitions.reserve(header.PartitionCount);
    for (DWORD index = 0; index < header.PartitionCount; ++index) {
        PARTITION_INFORMATION_EX entry{};
        const auto offset =
            kPartitionEntriesOffset + index * sizeof(PARTITION_INFORMATION_EX);
        std::memcpy(&entry, layout_buffer.data() + offset, sizeof(entry));
        // Skip empty partition table slots (zero length).
        if (entry.PartitionLength.QuadPart <= 0 || entry.PartitionNumber == 0) {
            continue;
        }
        layout.partitions.push_back(partition_from_info(entry, header.PartitionStyle));
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<WindowsPhysicalDiskLayout>
inspect_physical_disk_layout(const std::uint32_t disk_number) {
    auto handle = open_physical_drive(disk_number);
    if (!handle.valid()) {
        return base::Result<WindowsPhysicalDiskLayout>::failure(
            {base::ErrorCode::kNotFound, "physical disk could not be opened"});
    }
    WindowsPhysicalDiskLayout layout;
    layout.disk_number = disk_number;
    fill_disk_size(handle.get(), layout);
    if (layout.disk_size_bytes == 0) {
        return base::Result<WindowsPhysicalDiskLayout>::failure(
            {base::ErrorCode::kIoFailure, "physical disk size is unavailable"});
    }
    fill_storage_identity(handle.get(), layout);
    auto partitions = fill_partitions(handle.get(), layout);
    if (!partitions) {
        return base::Result<WindowsPhysicalDiskLayout>::failure(partitions.error());
    }
    auto raw = capture_raw_layout(handle.get(), layout);
    if (!raw) {
        return base::Result<WindowsPhysicalDiskLayout>::failure(raw.error());
    }
    return base::Result<WindowsPhysicalDiskLayout>::success(std::move(layout));
}

} // namespace aegra::adapters::windows_disk
