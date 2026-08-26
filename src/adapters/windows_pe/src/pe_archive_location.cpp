#include "aegra/adapters/windows_pe/archive_location.h"

#include "pe_pending_internal.h"

#include <winioctl.h>

#include <limits>
#include <utility>

namespace aegra::adapters::windows_pe {
namespace {

using detail::pe_store_error;
using detail::win32_error;

[[nodiscard]] base::Result<std::string> wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return base::Result<std::string>::success(std::string());
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is too long"));
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required <= 0) {
        return base::Result<std::string>::failure(
            pe_store_error(base::ErrorCode::kInvalidArgument, "path is not valid utf-16"));
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, utf8.data(),
                            required, nullptr, nullptr) != required) {
        return base::Result<std::string>::failure(win32_error(GetLastError(), "convert path"));
    }
    return base::Result<std::string>::success(std::move(utf8));
}

[[nodiscard]] base::Result<std::vector<std::uint32_t>>
volume_disk_numbers(const std::wstring& volume_guid_path) {
    // CreateFileW wants the volume path without the trailing slash.
    std::wstring device_path = volume_guid_path;
    while (!device_path.empty() && device_path.back() == L'\\') {
        device_path.pop_back();
    }
    detail::UniqueHandle volume(CreateFileW(device_path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                            OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        return base::Result<std::vector<std::uint32_t>>::failure(
            win32_error(GetLastError(), "open archive volume"));
    }
    // 16 extents cover every realistic spanned-volume layout for this check.
    struct ExtentsBuffer final {
        VOLUME_DISK_EXTENTS extents;
        DISK_EXTENT overflow[15];
    } buffer{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0, &buffer,
                         sizeof(buffer), &returned, nullptr)) {
        return base::Result<std::vector<std::uint32_t>>::failure(
            win32_error(GetLastError(), "query archive volume extents"));
    }
    std::vector<std::uint32_t> disks;
    disks.reserve(buffer.extents.NumberOfDiskExtents);
    for (DWORD index = 0; index < buffer.extents.NumberOfDiskExtents; ++index) {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic) Win32 flexible array.
        disks.push_back(buffer.extents.Extents[index].DiskNumber);
    }
    return base::Result<std::vector<std::uint32_t>>::success(std::move(disks));
}

} // namespace

base::Result<PeArchiveLocation> locate_pe_archive_layer(const std::string& absolute_path_utf8) {
    using Output = base::Result<PeArchiveLocation>;
    auto wide = detail::utf8_to_wide(absolute_path_utf8);
    if (!wide) {
        return Output::failure(wide.error());
    }
    const std::wstring& path = wide.value();
    wchar_t mount_root[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), mount_root, MAX_PATH)) {
        return Output::failure(win32_error(GetLastError(), "resolve archive volume root"));
    }
    wchar_t volume_guid[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(mount_root, volume_guid, MAX_PATH)) {
        return Output::failure(win32_error(GetLastError(), "resolve archive volume guid"));
    }
    const std::wstring root(mount_root);
    if (path.size() <= root.size()) {
        return Output::failure(pe_store_error(base::ErrorCode::kInvalidArgument,
                                              "archive path does not extend its volume root"));
    }
    const std::wstring relative = path.substr(root.size());
    auto guid_utf8 = wide_to_utf8(volume_guid);
    auto relative_utf8 = wide_to_utf8(relative);
    if (!guid_utf8 || !relative_utf8) {
        return Output::failure(!guid_utf8 ? guid_utf8.error() : relative_utf8.error());
    }
    auto disks = volume_disk_numbers(volume_guid);
    if (!disks) {
        return Output::failure(disks.error());
    }
    PeArchiveLocation location;
    location.volume_guid_utf8 = std::move(guid_utf8).value();
    location.relative_path_utf8 = std::move(relative_utf8).value();
    location.disk_numbers = std::move(disks).value();
    return Output::success(std::move(location));
}

} // namespace aegra::adapters::windows_pe
