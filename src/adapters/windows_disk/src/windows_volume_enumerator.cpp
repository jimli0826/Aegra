#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::windows_disk {
namespace {

class UniqueVolumeFind final {
  public:
    explicit UniqueVolumeFind(const HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueVolumeFind() {
        if (handle_ != INVALID_HANDLE_VALUE) {
            FindVolumeClose(handle_);
        }
    }

    UniqueVolumeFind(const UniqueVolumeFind&) = delete;
    UniqueVolumeFind& operator=(const UniqueVolumeFind&) = delete;
    UniqueVolumeFind(UniqueVolumeFind&&) = delete;
    UniqueVolumeFind& operator=(UniqueVolumeFind&&) = delete;

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

base::Result<std::string> to_utf8(const std::wstring& value) {
    if (value.empty()) {
        return base::Result<std::string>::success({});
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "WideCharToMultiByte"));
    }

    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.c_str(), input_size, output.data(),
                            required, nullptr, nullptr) == 0) {
        return base::Result<std::string>::failure(
            detail::win32_error(GetLastError(), "WideCharToMultiByte"));
    }
    return base::Result<std::string>::success(std::move(output));
}

std::vector<std::filesystem::path> mount_points(const wchar_t* volume_name) {
    DWORD required = 0;
    GetVolumePathNamesForVolumeNameW(volume_name, nullptr, 0, &required);
    if (required == 0) {
        return {};
    }

    std::vector<wchar_t> buffer(required);
    if (!GetVolumePathNamesForVolumeNameW(volume_name, buffer.data(),
                                          static_cast<DWORD>(buffer.size()), &required)) {
        return {};
    }

    std::vector<std::filesystem::path> result;
    const auto names = std::span<const wchar_t>(buffer);
    std::size_t begin = 0;
    while (begin < names.size() && names[begin] != L'\0') {
        const auto remaining = names.subspan(begin);
        const auto terminator = std::ranges::find(remaining, L'\0');
        if (terminator == remaining.end()) {
            break;
        }
        result.emplace_back(std::wstring(remaining.begin(), terminator));
        begin += static_cast<std::size_t>(std::distance(remaining.begin(), terminator)) + 1U;
    }
    return result;
}

bool load_filesystem_metadata(const wchar_t* volume_name, WindowsVolumeInfo& info) {
    constexpr DWORD kTextBufferSize = 256;
    std::array<wchar_t, kTextBufferSize> label{};
    std::array<wchar_t, kTextBufferSize> filesystem{};
    DWORD serial_number = 0;
    DWORD maximum_component_length = 0;
    DWORD flags = 0;
    if (!GetVolumeInformationW(volume_name, label.data(), kTextBufferSize, &serial_number,
                               &maximum_component_length, &flags, filesystem.data(),
                               kTextBufferSize)) {
        return false;
    }

    DWORD sectors_per_cluster = 0;
    DWORD bytes_per_sector = 0;
    DWORD free_clusters = 0;
    DWORD total_clusters = 0;
    if (!GetDiskFreeSpaceW(volume_name, &sectors_per_cluster, &bytes_per_sector, &free_clusters,
                           &total_clusters)) {
        return false;
    }

    auto utf8_label = to_utf8(label.data());
    auto utf8_filesystem = to_utf8(filesystem.data());
    const auto cluster_size = static_cast<std::uint64_t>(sectors_per_cluster) * bytes_per_sector;
    if (!utf8_label || !utf8_filesystem ||
        cluster_size > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }

    info.label = std::move(utf8_label).value();
    info.filesystem = std::move(utf8_filesystem).value();
    info.cluster_size_bytes = static_cast<std::uint32_t>(cluster_size);
    info.filesystem_metadata_available = true;
    return true;
}

std::wstring volume_device_path(const std::wstring_view volume_name) {
    std::wstring path(volume_name);
    if (!path.empty() && path.back() == L'\\') {
        path.pop_back();
    }
    return path;
}

bool parse_extents(const std::vector<std::byte>& buffer, const DWORD bytes_returned,
                   WindowsVolumeInfo& info) {
    constexpr auto kHeaderSize = offsetof(VOLUME_DISK_EXTENTS, Extents);
    if (bytes_returned < kHeaderSize || bytes_returned > buffer.size()) {
        return false;
    }
    DWORD extent_count = 0;
    std::memcpy(&extent_count, buffer.data(), sizeof(extent_count));
    const auto extent_bytes = static_cast<std::uint64_t>(extent_count) * sizeof(DISK_EXTENT);
    if (extent_bytes > bytes_returned - kHeaderSize) {
        return false;
    }

    std::vector<WindowsVolumeExtent> extents;
    extents.reserve(extent_count);
    const auto returned_data = std::span<const std::byte>(buffer).first(bytes_returned);
    for (DWORD index = 0; index < extent_count; ++index) {
        DISK_EXTENT extent{};
        const auto offset = kHeaderSize + static_cast<std::size_t>(index) * sizeof(extent);
        const auto encoded = returned_data.subspan(offset, sizeof(extent));
        std::memcpy(&extent, encoded.data(), sizeof(extent));
        if (extent.StartingOffset.QuadPart < 0 || extent.ExtentLength.QuadPart < 0) {
            return false;
        }
        extents.push_back(WindowsVolumeExtent{
            extent.DiskNumber,
            static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
            static_cast<std::uint64_t>(extent.ExtentLength.QuadPart),
        });
    }
    info.extents = std::move(extents);
    info.disk_extents_available = true;
    return true;
}

void load_volume_size(const HANDLE handle, WindowsVolumeInfo& info) {
    GET_LENGTH_INFORMATION length{};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                         &bytes_returned, nullptr) ||
        bytes_returned < sizeof(length) || length.Length.QuadPart < 0) {
        return;
    }
    info.total_size_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    info.volume_size_available = true;
}

bool load_disk_extents(const wchar_t* volume_name, WindowsVolumeInfo& info) {
    const auto device_path = volume_device_path(volume_name);
    detail::UniqueHandle handle(CreateFileW(device_path.c_str(), 0,
                                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                            nullptr, OPEN_EXISTING, 0, nullptr));
    if (!handle.valid()) {
        return false;
    }
    load_volume_size(handle.get(), info);

    constexpr std::size_t kMaximumBufferSize = std::size_t{1024} * 1024U;
    std::vector<std::byte> buffer(sizeof(VOLUME_DISK_EXTENTS) + 8U * sizeof(DISK_EXTENT));
    while (buffer.size() <= kMaximumBufferSize) {
        DWORD bytes_returned = 0;
        if (DeviceIoControl(handle.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS, nullptr, 0,
                            buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_returned,
                            nullptr)) {
            return parse_extents(buffer, bytes_returned, info);
        }
        if (GetLastError() != ERROR_MORE_DATA || buffer.size() == kMaximumBufferSize) {
            return false;
        }
        buffer.resize((std::min)(buffer.size() * 2U, kMaximumBufferSize));
    }
    return false;
}

WindowsVolumeInfo inspect_volume(const wchar_t* volume_name) {
    WindowsVolumeInfo info;
    info.volume_guid_path = volume_name;
    info.mount_points = mount_points(volume_name);
    load_filesystem_metadata(volume_name, info);
    load_disk_extents(volume_name, info);
    return info;
}

} // namespace

base::Result<std::vector<WindowsVolumeInfo>> WindowsVolumeEnumerator::enumerate() {
    constexpr DWORD kVolumeNameSize = MAX_PATH + 1;
    std::array<wchar_t, kVolumeNameSize> volume_name{};
    const auto find_handle = FindFirstVolumeW(volume_name.data(), kVolumeNameSize);
    if (find_handle == INVALID_HANDLE_VALUE) {
        return base::Result<std::vector<WindowsVolumeInfo>>::failure(
            detail::win32_error(GetLastError(), "FindFirstVolumeW"));
    }
    const UniqueVolumeFind find_guard(find_handle);

    std::vector<WindowsVolumeInfo> volumes;
    for (;;) {
        volumes.push_back(inspect_volume(volume_name.data()));
        if (FindNextVolumeW(find_handle, volume_name.data(), kVolumeNameSize)) {
            continue;
        }
        const auto error = GetLastError();
        if (error == ERROR_NO_MORE_FILES) {
            break;
        }
        return base::Result<std::vector<WindowsVolumeInfo>>::failure(
            detail::win32_error(error, "FindNextVolumeW"));
    }
    return base::Result<std::vector<WindowsVolumeInfo>>::success(std::move(volumes));
}

} // namespace aegra::adapters::windows_disk
