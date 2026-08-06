#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <winioctl.h>

#include <array>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <vector>

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

[[nodiscard]] base::Result<void>
write_exact_at(const HANDLE handle, const std::uint64_t offset,
               const std::span<const std::byte> source) {
    if (source.empty()) {
        return base::Result<void>::success();
    }
    if (source.size() > (std::numeric_limits<DWORD>::max)()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "raw layout write size is too large"});
    }
    LARGE_INTEGER move{};
    move.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, move, nullptr, FILE_BEGIN)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "raw layout write seek failed"});
    }
    DWORD written = 0;
    if (!WriteFile(handle, source.data(), static_cast<DWORD>(source.size()), &written, nullptr) ||
        written != source.size()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "raw layout write failed"});
    }
    return base::Result<void>::success();
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

[[nodiscard]] base::Result<std::uint32_t> disk_bytes_per_sector(const HANDLE handle) {
    DISK_GEOMETRY geometry{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &geometry,
                         sizeof(geometry), &returned, nullptr) ||
        returned < sizeof(geometry) || geometry.BytesPerSector == 0) {
        return base::Result<std::uint32_t>::failure(
            {base::ErrorCode::kIoFailure, "target disk sector size is unavailable"});
    }
    return base::Result<std::uint32_t>::success(geometry.BytesPerSector);
}

void update_disk_properties(const HANDLE handle) noexcept {
    DWORD returned = 0;
    DeviceIoControl(handle, IOCTL_DISK_UPDATE_PROPERTIES, nullptr, 0, nullptr, 0, &returned,
                    nullptr);
}

[[nodiscard]] base::Result<std::uint32_t>
volume_disk_number(const std::filesystem::path& volume_guid_path) {
    auto path = volume_guid_path.native();
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    detail::UniqueHandle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) {
        return base::Result<std::uint32_t>::failure(
            detail::win32_error(GetLastError(), "CreateFileW volume for disk number"));
    }
    VOLUME_DISK_EXTENTS extents{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &extents,
                         sizeof(extents), &returned, nullptr) ||
        returned < sizeof(extents) || extents.NumberOfDiskExtents == 0) {
        // Retry with heap buffer when the volume spans multiple disks.
        std::vector<std::byte> buffer(sizeof(VOLUME_DISK_EXTENTS) +
                                      sizeof(DISK_EXTENT) * 16U);
        if (!DeviceIoControl(handle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                             buffer.data(), static_cast<DWORD>(buffer.size()), &returned,
                             nullptr) ||
            returned < sizeof(VOLUME_DISK_EXTENTS)) {
            return base::Result<std::uint32_t>::failure(
                {base::ErrorCode::kIoFailure, "volume disk extents are unavailable"});
        }
        const auto* multi = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
        if (multi->NumberOfDiskExtents == 0) {
            return base::Result<std::uint32_t>::failure(
                {base::ErrorCode::kIoFailure, "volume has no disk extents"});
        }
        return base::Result<std::uint32_t>::success(multi->Extents[0].DiskNumber);
    }
    return base::Result<std::uint32_t>::success(extents.Extents[0].DiskNumber);
}

} // namespace

base::Result<bool> is_system_physical_disk(const std::uint32_t disk_number) {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    const auto length = GetWindowsDirectoryW(windows_directory.data(),
                                             static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return base::Result<bool>::failure(
            detail::win32_error(GetLastError(), "GetWindowsDirectoryW"));
    }
    std::array<wchar_t, MAX_PATH + 1> mount_point{};
    if (!GetVolumePathNameW(windows_directory.data(), mount_point.data(),
                            static_cast<DWORD>(mount_point.size()))) {
        return base::Result<bool>::failure(
            detail::win32_error(GetLastError(), "GetVolumePathNameW"));
    }
    std::array<wchar_t, 64> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(mount_point.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
        return base::Result<bool>::failure(
            detail::win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
    }
    auto system_disk = volume_disk_number(volume_name.data());
    if (!system_disk) {
        return base::Result<bool>::failure(system_disk.error());
    }
    return base::Result<bool>::success(system_disk.value() == disk_number);
}

base::Result<void> prepare_target_disk_for_raw_restore(const std::uint32_t disk_number,
                                                       const std::uint64_t source_disk_size_bytes,
                                                       const std::uint32_t source_bytes_per_sector) {
    auto system = is_system_physical_disk(disk_number);
    if (!system) {
        return base::Result<void>::failure(system.error());
    }
    if (system.value()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "online system disk restore is forbidden"});
    }
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk"));
    }
    auto length = disk_length_bytes(handle.get());
    if (!length) {
        return base::Result<void>::failure(length.error());
    }
    if (length.value() < source_disk_size_bytes) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInsufficientSpace, "target disk is smaller than source disk"});
    }
    if (source_bytes_per_sector != 0) {
        auto sector = disk_bytes_per_sector(handle.get());
        if (!sector) {
            return base::Result<void>::failure(sector.error());
        }
        if (sector.value() != source_bytes_per_sector) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "target disk sector size does not match source"});
        }
    }
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_DELETE_DRIVE_LAYOUT, nullptr, 0, nullptr, 0,
                         &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_DELETE_DRIVE_LAYOUT"));
    }
    FlushFileBuffers(handle.get());
    update_disk_properties(handle.get());
    return base::Result<void>::success();
}

base::Result<void>
rebuild_partition_table_from_raw_layout(const std::uint32_t disk_number,
                                        const std::uint32_t source_bytes_per_sector,
                                        const std::uint64_t source_disk_size_bytes,
                                        const WindowsRawDiskLayout& raw_layout,
                                        const std::string& partition_style) {
    if (raw_layout.mbr_sector.empty() && partition_style != "RAW") {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "raw layout MBR sector is missing"});
    }
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk for layout"));
    }
    auto length = disk_length_bytes(handle.get());
    if (!length) {
        return base::Result<void>::failure(length.error());
    }
    const auto sector = source_bytes_per_sector == 0 ? 512U : source_bytes_per_sector;
    if (!raw_layout.mbr_sector.empty()) {
        auto written = write_exact_at(handle.get(), 0, raw_layout.mbr_sector);
        if (!written) {
            return written;
        }
    }
    if (partition_style == "GPT") {
        if (!raw_layout.gpt_primary_header.empty()) {
            auto written = write_exact_at(handle.get(), sector, raw_layout.gpt_primary_header);
            if (!written) {
                return written;
            }
        }
        if (!raw_layout.gpt_partition_entries.empty()) {
            auto written =
                write_exact_at(handle.get(), static_cast<std::uint64_t>(sector) * 2U,
                               raw_layout.gpt_partition_entries);
            if (!written) {
                return written;
            }
        }
        const auto target_size = length.value();
        if (!raw_layout.gpt_backup_header.empty() && target_size >= sector) {
            // Prefer target end-of-disk placement so larger targets remain valid.
            const auto backup_header_offset = target_size - sector;
            auto written =
                write_exact_at(handle.get(), backup_header_offset, raw_layout.gpt_backup_header);
            if (!written) {
                return written;
            }
            if (!raw_layout.gpt_backup_entries.empty() &&
                backup_header_offset >= raw_layout.gpt_backup_entries.size()) {
                auto backup_entries_offset =
                    backup_header_offset - raw_layout.gpt_backup_entries.size();
                backup_entries_offset = (backup_entries_offset / sector) * sector;
                static_cast<void>(write_exact_at(handle.get(), backup_entries_offset,
                                                 raw_layout.gpt_backup_entries));
            }
        } else if (!raw_layout.gpt_backup_header.empty() &&
                   source_disk_size_bytes >= sector) {
            const auto backup_header_offset = source_disk_size_bytes - sector;
            static_cast<void>(
                write_exact_at(handle.get(), backup_header_offset, raw_layout.gpt_backup_header));
        }
    }
    if (!FlushFileBuffers(handle.get())) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "FlushFileBuffers partition table"));
    }
    update_disk_properties(handle.get());
    return base::Result<void>::success();
}

base::Result<void> bring_target_disk_online(const std::uint32_t disk_number) {
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk online"));
    }
    // GET attributes then clear OFFLINE / READ_ONLY.
    GET_DISK_ATTRIBUTES attributes{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes,
                         sizeof(attributes), &returned, nullptr)) {
        // Older stacks may lack the IOCTL; treat as non-fatal after layout rebuild.
        update_disk_properties(handle.get());
        return base::Result<void>::success();
    }
    SET_DISK_ATTRIBUTES set_attributes{};
    set_attributes.Version = sizeof(set_attributes);
    set_attributes.Persist = TRUE;
    set_attributes.AttributesMask =
        DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    set_attributes.Attributes = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_SET_DISK_ATTRIBUTES, &set_attributes,
                         sizeof(set_attributes), nullptr, 0, &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_SET_DISK_ATTRIBUTES"));
    }
    update_disk_properties(handle.get());
    return base::Result<void>::success();
}

base::Result<std::uint32_t>
physical_disk_number_for_path(const std::filesystem::path& path) {
    std::array<wchar_t, MAX_PATH + 1> mount_point{};
    if (!GetVolumePathNameW(path.c_str(), mount_point.data(),
                            static_cast<DWORD>(mount_point.size()))) {
        return base::Result<std::uint32_t>::failure(
            detail::win32_error(GetLastError(), "GetVolumePathNameW"));
    }
    std::array<wchar_t, 64> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(mount_point.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
        return base::Result<std::uint32_t>::failure(
            detail::win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
    }
    return volume_disk_number(volume_name.data());
}

} // namespace aegra::adapters::windows_disk
