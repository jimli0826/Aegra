#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <winioctl.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <thread>
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

base::Result<void> validate_target_disk_for_raw_restore(const TargetDiskGeometryCheck& check) {
    auto system = is_system_physical_disk(check.disk_number);
    if (!system) {
        return base::Result<void>::failure(system.error());
    }
    if (system.value()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "online system disk restore is forbidden"});
    }
    auto handle = open_disk_read_write(check.disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk"));
    }
    auto length = disk_length_bytes(handle.get());
    if (!length) {
        return base::Result<void>::failure(length.error());
    }
    if (length.value() < check.source_disk_size_bytes) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInsufficientSpace, "target disk is smaller than source disk"});
    }
    if (check.source_bytes_per_sector == 0) {
        return base::Result<void>::success();
    }
    auto sector = disk_bytes_per_sector(handle.get());
    if (!sector) {
        return base::Result<void>::failure(sector.error());
    }
    if (sector.value() != check.source_bytes_per_sector) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "target disk sector size does not match source"});
    }
    return base::Result<void>::success();
}

base::Result<void> delete_target_disk_drive_layout(const std::uint32_t disk_number) {
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk delete layout"));
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

base::Result<void> prepare_target_disk_for_raw_restore(const std::uint32_t disk_number,
                                                       const std::uint64_t source_disk_size_bytes,
                                                       const std::uint32_t source_bytes_per_sector) {
    TargetDiskGeometryCheck check{disk_number, source_disk_size_bytes, source_bytes_per_sector};
    auto validated = validate_target_disk_for_raw_restore(check);
    if (!validated) {
        return validated;
    }
    return delete_target_disk_drive_layout(disk_number);
}

namespace {

[[nodiscard]] base::Result<void>
write_gpt_layout(const HANDLE handle, const std::uint32_t sector, const std::uint64_t target_size,
                 const WindowsRawDiskLayout& raw_layout) {
    if (raw_layout.gpt_primary_header.empty() || raw_layout.gpt_partition_entries.empty() ||
        raw_layout.gpt_backup_header.empty() || raw_layout.gpt_backup_entries.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT raw layout is incomplete for write"});
    }
    auto primary = write_exact_at(handle, sector, raw_layout.gpt_primary_header);
    if (!primary) {
        return primary;
    }
    auto entries =
        write_exact_at(handle, static_cast<std::uint64_t>(sector) * 2U,
                       raw_layout.gpt_partition_entries);
    if (!entries) {
        return entries;
    }
    if (target_size < sector) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "target disk too small for GPT backup header"});
    }
    const auto backup_header_offset = target_size - sector;
    auto backup = write_exact_at(handle, backup_header_offset, raw_layout.gpt_backup_header);
    if (!backup) {
        return backup;
    }
    std::uint64_t backup_entries_offset = 0;
    if (raw_layout.gpt_backup_header.size() >= 80) {
        std::uint64_t entry_lba = 0;
        std::memcpy(&entry_lba, raw_layout.gpt_backup_header.data() + 72, sizeof(entry_lba));
        if (entry_lba > 0 && entry_lba < target_size / sector) {
            backup_entries_offset = entry_lba * static_cast<std::uint64_t>(sector);
        }
    }
    if (backup_entries_offset == 0 &&
        backup_header_offset >= raw_layout.gpt_backup_entries.size()) {
        backup_entries_offset = backup_header_offset - raw_layout.gpt_backup_entries.size();
        backup_entries_offset = (backup_entries_offset / sector) * sector;
    }
    if (backup_entries_offset == 0 ||
        backup_entries_offset + raw_layout.gpt_backup_entries.size() > target_size) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "GPT backup entry array placement is invalid"});
    }
    return write_exact_at(handle, backup_entries_offset, raw_layout.gpt_backup_entries);
}

} // namespace

base::Result<void>
rebuild_partition_table_from_raw_layout(const RebuildPartitionTableRequest& request) {
    if (request.raw_layout == nullptr) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "raw layout pointer is null"});
    }
    const auto& raw_layout = *request.raw_layout;
    if (raw_layout.mbr_sector.empty() && request.partition_style != "RAW") {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "raw layout MBR sector is missing"});
    }
    auto handle = open_disk_read_write(request.disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk for layout"));
    }
    auto length = disk_length_bytes(handle.get());
    if (!length) {
        return base::Result<void>::failure(length.error());
    }
    const auto sector =
        request.source_bytes_per_sector == 0 ? 512U : request.source_bytes_per_sector;
    if (!raw_layout.mbr_sector.empty()) {
        auto written = write_exact_at(handle.get(), 0, raw_layout.mbr_sector);
        if (!written) {
            return written;
        }
    }
    if (request.partition_style == "GPT") {
        auto gpt = write_gpt_layout(handle.get(), sector, length.value(), raw_layout);
        if (!gpt) {
            return gpt;
        }
    }
    if (!FlushFileBuffers(handle.get())) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "FlushFileBuffers partition table"));
    }
    update_disk_properties(handle.get());
    return base::Result<void>::success();
}

namespace {

[[nodiscard]] bool volume_is_on_disk(const std::wstring& volume_open_path,
                                     const std::uint32_t disk_number) noexcept {
    detail::UniqueHandle volume(CreateFileW(volume_open_path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        return false;
    }
    std::vector<std::byte> buffer(sizeof(VOLUME_DISK_EXTENTS) + sizeof(DISK_EXTENT) * 16U);
    DWORD returned = 0;
    if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                         buffer.data(), static_cast<DWORD>(buffer.size()), &returned, nullptr) ||
        returned < sizeof(VOLUME_DISK_EXTENTS)) {
        return false;
    }
    const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(buffer.data());
    for (DWORD i = 0; i < extents->NumberOfDiskExtents; ++i) {
        if (extents->Extents[i].DiskNumber == disk_number) {
            return true;
        }
    }
    return false;
}

// Best-effort: bring every volume on the disk online so Disk Management shows them mounted.
void online_volumes_on_disk(const std::uint32_t disk_number) noexcept {
    std::array<wchar_t, MAX_PATH> name{};
    const auto find = FindFirstVolumeW(name.data(), static_cast<DWORD>(name.size()));
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    do {
        std::wstring open_path(name.data());
        if (!open_path.empty() && open_path.back() == L'\\') {
            open_path.pop_back();
        }
        if (!volume_is_on_disk(open_path, disk_number)) {
            continue;
        }
        detail::UniqueHandle volume(CreateFileW(open_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                OPEN_EXISTING, 0, nullptr));
        if (!volume.valid()) {
            continue;
        }
        DWORD returned = 0;
        DeviceIoControl(volume.get(), IOCTL_VOLUME_ONLINE, nullptr, 0, nullptr, 0, &returned,
                        nullptr);
    } while (FindNextVolumeW(find, name.data(), static_cast<DWORD>(name.size())));
    FindVolumeClose(find);
}

[[nodiscard]] base::Result<void> set_disk_offline_state(const HANDLE handle,
                                                        const bool offline) {
    SET_DISK_ATTRIBUTES set_attributes{};
    set_attributes.Version = sizeof(set_attributes);
    set_attributes.Persist = TRUE;
    set_attributes.AttributesMask = DISK_ATTRIBUTE_OFFLINE | DISK_ATTRIBUTE_READ_ONLY;
    set_attributes.Attributes = offline ? DISK_ATTRIBUTE_OFFLINE : 0;
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_SET_DISK_ATTRIBUTES, &set_attributes,
                         sizeof(set_attributes), nullptr, 0, &returned, nullptr)) {
        return base::Result<void>::failure(detail::win32_error(
            GetLastError(), offline ? "IOCTL_DISK_SET_DISK_ATTRIBUTES offline"
                                    : "IOCTL_DISK_SET_DISK_ATTRIBUTES online"));
    }
    update_disk_properties(handle);
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> clear_offline_with_retry(const HANDLE handle) {
    auto cleared = set_disk_offline_state(handle, false);
    if (cleared) {
        return cleared;
    }
    // Retry once after property refresh — attribute IOCTL can race with PnP.
    update_disk_properties(handle);
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return set_disk_offline_state(handle, false);
}

[[nodiscard]] base::Result<void> verify_disk_online(const HANDLE handle) {
    GET_DISK_ATTRIBUTES attributes{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes,
                         sizeof(attributes), &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GET_DISK_ATTRIBUTES after online"));
    }
    if ((attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "target disk is still offline after online request"});
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> bring_target_disk_online(const std::uint32_t disk_number) {
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk online"));
    }
    auto cleared = clear_offline_with_retry(handle.get());
    if (!cleared) {
        return cleared;
    }
    handle.reset();
    // Allow mount manager to enumerate the new GPT before volume ONLINE.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    online_volumes_on_disk(disk_number);
    // Second pass: some stacks re-apply OFFLINE until volumes settle.
    handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk online verify"));
    }
    GET_DISK_ATTRIBUTES attributes{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes,
                         sizeof(attributes), &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GET_DISK_ATTRIBUTES online pass"));
    }
    if ((attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) != 0) {
        auto again = set_disk_offline_state(handle.get(), false);
        if (!again) {
            return again;
        }
    }
    update_disk_properties(handle.get());
    online_volumes_on_disk(disk_number);
    auto verified = verify_disk_online(handle.get());
    if (!verified) {
        return verified;
    }
    return base::Result<void>::success();
}

base::Result<void> set_target_disk_offline(const std::uint32_t disk_number) {
    auto handle = open_disk_read_write(disk_number);
    if (!handle.valid()) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "CreateFileW target disk offline"));
    }
    // Fail-closed: raw PhysicalDrive writes must not proceed while the disk is online.
    auto set = set_disk_offline_state(handle.get(), true);
    if (!set) {
        update_disk_properties(handle.get());
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        set = set_disk_offline_state(handle.get(), true);
        if (!set) {
            return set;
        }
    }
    GET_DISK_ATTRIBUTES attributes{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.get(), IOCTL_DISK_GET_DISK_ATTRIBUTES, nullptr, 0, &attributes,
                         sizeof(attributes), &returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GET_DISK_ATTRIBUTES after offline"));
    }
    if ((attributes.Attributes & DISK_ATTRIBUTE_OFFLINE) == 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "target disk is still online after offline request"});
    }
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
