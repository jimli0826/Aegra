#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <winioctl.h>

#include <array>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

[[nodiscard]] detail::UniqueHandle open_physical_drive(const std::uint32_t disk_number) {
    const auto path = std::wstring(LR"(\\.\PhysicalDrive)") + std::to_wstring(disk_number);
    detail::UniqueHandle handle(CreateFileW(path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.valid()) {
        return handle;
    }
    return detail::UniqueHandle(CreateFileW(path.c_str(), GENERIC_READ,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
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

[[nodiscard]] base::Result<void> fill_partitions(const HANDLE handle,
                                                 WindowsPhysicalDiskLayout& layout) {
    constexpr DWORD kMaxPartitions = 128;
    const auto layout_bytes =
        sizeof(DRIVE_LAYOUT_INFORMATION_EX) + sizeof(PARTITION_INFORMATION_EX) * kMaxPartitions;
    std::vector<std::byte> layout_buffer(layout_bytes);
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_LAYOUT_EX, nullptr, 0, layout_buffer.data(),
                         static_cast<DWORD>(layout_buffer.size()), &bytes_returned, nullptr) ||
        bytes_returned < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk partition layout is unavailable"});
    }
    DRIVE_LAYOUT_INFORMATION_EX header{};
    std::memcpy(&header, layout_buffer.data(), sizeof(header));
    layout.partition_style = partition_style_name(header.PartitionStyle);
    if (header.PartitionCount > kMaxPartitions) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk reports too many partitions"});
    }
    const auto entries_bytes =
        static_cast<std::size_t>(header.PartitionCount) * sizeof(PARTITION_INFORMATION_EX);
    if (sizeof(DRIVE_LAYOUT_INFORMATION_EX) + entries_bytes > bytes_returned) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "physical disk partition layout is truncated"});
    }
    layout.partitions.reserve(header.PartitionCount);
    for (DWORD index = 0; index < header.PartitionCount; ++index) {
        PARTITION_INFORMATION_EX entry{};
        const auto offset =
            sizeof(DRIVE_LAYOUT_INFORMATION_EX) + index * sizeof(PARTITION_INFORMATION_EX);
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
    return base::Result<WindowsPhysicalDiskLayout>::success(std::move(layout));
}

} // namespace aegra::adapters::windows_disk
