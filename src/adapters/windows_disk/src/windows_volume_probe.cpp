#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <utility>

namespace aegra::adapters::windows_disk {
namespace {

[[nodiscard]] std::filesystem::path open_path_without_trailing_slash(
    const std::filesystem::path& volume_guid_path) {
    auto value = volume_guid_path.native();
    if (!value.empty() && (value.back() == L'\\' || value.back() == L'/')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] base::Result<detail::UniqueHandle>
open_volume_read(const std::filesystem::path& volume_guid_path) {
    if (!WindowsBlockSink::is_canonical_volume_guid_path(volume_guid_path)) {
        return base::Result<detail::UniqueHandle>::failure(
            {base::ErrorCode::kInvalidArgument, "target is not a canonical Volume GUID path"});
    }
    detail::UniqueHandle handle(CreateFileW(
        open_path_without_trailing_slash(volume_guid_path).c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
    if (!handle.valid()) {
        return base::Result<detail::UniqueHandle>::failure(
            detail::win32_error(GetLastError(), "CreateFileW"));
    }
    return base::Result<detail::UniqueHandle>::success(std::move(handle));
}

[[nodiscard]] base::Result<DWORD> device_control(const HANDLE handle, const DWORD control,
                                                 void* output, const DWORD output_size,
                                                 const char* operation, void* input = nullptr,
                                                 const DWORD input_size = 0) {
    detail::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<DWORD>::failure(detail::win32_error(GetLastError(), "CreateEventW"));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const auto started =
        DeviceIoControl(handle, control, input, input_size, output, output_size, nullptr,
                        &overlapped);
    const auto error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && error != ERROR_IO_PENDING) {
        return base::Result<DWORD>::failure(detail::win32_error(error, operation));
    }
    if (WaitForSingleObject(event.get(), INFINITE) != WAIT_OBJECT_0 ||
        !GetOverlappedResult(handle, &overlapped, &returned, FALSE)) {
        return base::Result<DWORD>::failure(detail::win32_error(GetLastError(), operation));
    }
    return base::Result<DWORD>::success(returned);
}

[[nodiscard]] base::Result<std::uint64_t> query_capacity_bytes(const HANDLE handle) {
    GET_LENGTH_INFORMATION length{};
    auto result = device_control(handle, IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length),
                                 "IOCTL_DISK_GET_LENGTH_INFO");
    if (!result || result.value() < sizeof(length) || length.Length.QuadPart <= 0) {
        return result ? base::Result<std::uint64_t>::failure(
                            {base::ErrorCode::kIoFailure, "volume length is invalid"})
                      : base::Result<std::uint64_t>::failure(result.error());
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(length.Length.QuadPart));
}

[[nodiscard]] base::Result<ports::BlockDeviceGeometry>
query_geometry(const HANDLE handle, const std::uint64_t capacity_bytes) {
    ports::BlockDeviceGeometry geometry;
    geometry.capacity_bytes = capacity_bytes;
    DISK_GEOMETRY disk{};
    auto result = device_control(handle, IOCTL_DISK_GET_DRIVE_GEOMETRY, &disk, sizeof(disk),
                                 "IOCTL_DISK_GET_DRIVE_GEOMETRY");
    if (!result || result.value() < sizeof(disk) || disk.BytesPerSector == 0) {
        return result ? base::Result<ports::BlockDeviceGeometry>::failure(
                            {base::ErrorCode::kIoFailure, "volume geometry is incomplete"})
                      : base::Result<ports::BlockDeviceGeometry>::failure(result.error());
    }
    geometry.logical_sector_size = disk.BytesPerSector;
    geometry.physical_sector_size = disk.BytesPerSector;
    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
    auto alignment_result = device_control(handle, IOCTL_STORAGE_QUERY_PROPERTY, &alignment,
                                           sizeof(alignment), "IOCTL_STORAGE_QUERY_PROPERTY",
                                           &query, sizeof(query));
    if (!alignment_result || alignment_result.value() < sizeof(alignment) ||
        alignment.BytesPerLogicalSector == 0 || alignment.BytesPerPhysicalSector == 0) {
        return alignment_result ? base::Result<ports::BlockDeviceGeometry>::failure(
                                      {base::ErrorCode::kIoFailure,
                                       "volume sector alignment is incomplete"})
                                : base::Result<ports::BlockDeviceGeometry>::failure(
                                      alignment_result.error());
    }
    geometry.logical_sector_size = alignment.BytesPerLogicalSector;
    geometry.physical_sector_size = alignment.BytesPerPhysicalSector;
    const auto logical = geometry.logical_sector_size;
    const auto physical = geometry.physical_sector_size;
    if ((logical & (logical - 1U)) != 0 || (physical & (physical - 1U)) != 0 ||
        physical < logical || capacity_bytes % logical != 0) {
        return base::Result<ports::BlockDeviceGeometry>::failure(
            {base::ErrorCode::kIoFailure, "volume sector geometry is invalid"});
    }
    return base::Result<ports::BlockDeviceGeometry>::success(geometry);
}

[[nodiscard]] base::Result<bool> query_volume_dirty(const HANDLE handle) {
    ULONG dirty_flags = 0;
    auto result = device_control(handle, FSCTL_IS_VOLUME_DIRTY, &dirty_flags, sizeof(dirty_flags),
                                 "FSCTL_IS_VOLUME_DIRTY");
    if (!result || result.value() < sizeof(dirty_flags)) {
        return result ? base::Result<bool>::failure(
                            {base::ErrorCode::kIoFailure, "FSCTL_IS_VOLUME_DIRTY returned short"})
                      : base::Result<bool>::failure(result.error());
    }
    constexpr ULONG kVolumeIsDirty = 0x1;
    return base::Result<bool>::success((dirty_flags & kVolumeIsDirty) != 0);
}

} // namespace

base::Result<ports::BlockDeviceGeometry>
probe_volume_block_geometry(const std::filesystem::path& volume_guid_path) {
    auto handle = open_volume_read(volume_guid_path);
    if (!handle) {
        return base::Result<ports::BlockDeviceGeometry>::failure(handle.error());
    }
    auto capacity = query_capacity_bytes(handle.value().get());
    if (!capacity) {
        return base::Result<ports::BlockDeviceGeometry>::failure(capacity.error());
    }
    return query_geometry(handle.value().get(), capacity.value());
}

base::Result<VolumeShrinkPostcheckSnapshot>
query_volume_shrink_postcheck(const std::filesystem::path& volume_guid_path) {
    VolumeShrinkPostcheckSnapshot snapshot;
    auto handle = open_volume_read(volume_guid_path);
    if (!handle) {
        return base::Result<VolumeShrinkPostcheckSnapshot>::failure(handle.error());
    }
    auto capacity = query_capacity_bytes(handle.value().get());
    if (!capacity) {
        return base::Result<VolumeShrinkPostcheckSnapshot>::failure(capacity.error());
    }
    auto geometry = query_geometry(handle.value().get(), capacity.value());
    if (!geometry) {
        return base::Result<VolumeShrinkPostcheckSnapshot>::failure(geometry.error());
    }
    auto dirty = query_volume_dirty(handle.value().get());
    if (!dirty) {
        return base::Result<VolumeShrinkPostcheckSnapshot>::failure(dirty.error());
    }
    snapshot.query_succeeded = true;
    snapshot.volume_dirty = dirty.value();
    snapshot.capacity_bytes = capacity.value();
    snapshot.bytes_per_sector = geometry.value().logical_sector_size;
    return base::Result<VolumeShrinkPostcheckSnapshot>::success(snapshot);
}

} // namespace aegra::adapters::windows_disk
