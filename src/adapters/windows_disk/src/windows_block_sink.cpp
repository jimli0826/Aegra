#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::adapters::windows_disk {
namespace {

constexpr std::wstring_view kVolumePrefix = LR"(\\?\Volume{)";
constexpr std::wstring_view kDevicePrefix = LR"(\\.\)";
constexpr std::wstring_view kGlobalPrefix = LR"(\\?\GLOBALROOT\)";
constexpr std::wstring_view kPhysicalDrivePrefix = LR"(\\.\PhysicalDrive)";

bool equal_case_insensitive(const std::wstring_view left,
                            const std::wstring_view right) noexcept {
    return left.size() == right.size() &&
           std::ranges::equal(left, right, [](const wchar_t first, const wchar_t second) {
               return std::towlower(first) == std::towlower(second);
           });
}

bool starts_with_case_insensitive(const std::wstring_view value,
                                  const std::wstring_view prefix) noexcept {
    return value.size() >= prefix.size() &&
           equal_case_insensitive(value.substr(0, prefix.size()), prefix);
}

bool is_hex(const wchar_t value) noexcept {
    return (value >= L'0' && value <= L'9') || (value >= L'a' && value <= L'f') ||
           (value >= L'A' && value <= L'F');
}

bool valid_guid_body(const std::wstring_view body) noexcept {
    constexpr std::array<std::size_t, 4> hyphens{8, 13, 18, 23};
    if (body.size() != 36) {
        return false;
    }
    for (std::size_t index = 0; index < body.size(); ++index) {
        const bool expected_hyphen = std::ranges::find(hyphens, index) != hyphens.end();
        if ((expected_hyphen && body[index] != L'-') || (!expected_hyphen && !is_hex(body[index]))) {
            return false;
        }
    }
    return true;
}

bool is_device_path(const std::wstring_view path) noexcept {
    return starts_with_case_insensitive(path, kDevicePrefix) ||
           starts_with_case_insensitive(path, kGlobalPrefix) ||
           starts_with_case_insensitive(path, kVolumePrefix);
}

base::Result<std::filesystem::path> system_volume_path() {
    std::array<wchar_t, MAX_PATH + 1> windows_directory{};
    const auto length = GetWindowsDirectoryW(windows_directory.data(),
                                             static_cast<UINT>(windows_directory.size()));
    if (length == 0 || length >= windows_directory.size()) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetWindowsDirectoryW"));
    }
    std::array<wchar_t, MAX_PATH + 1> mount_point{};
    if (!GetVolumePathNameW(windows_directory.data(), mount_point.data(),
                            static_cast<DWORD>(mount_point.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumePathNameW"));
    }
    std::array<wchar_t, 64> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(mount_point.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
    }
    return base::Result<std::filesystem::path>::success(volume_name.data());
}

base::Result<std::filesystem::path>
path_volume(const std::filesystem::path& source) {
    std::array<wchar_t, MAX_PATH + 1> mount_point{};
    if (!GetVolumePathNameW(source.c_str(), mount_point.data(),
                            static_cast<DWORD>(mount_point.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumePathNameW"));
    }
    std::array<wchar_t, 64> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(mount_point.data(), volume_name.data(),
                                           static_cast<DWORD>(volume_name.size()))) {
        return base::Result<std::filesystem::path>::failure(
            detail::win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW"));
    }
    return base::Result<std::filesystem::path>::success(volume_name.data());
}

base::Result<void> validate_protected_sources_volume(const WindowsBlockSinkOpenRequest& request) {
    if (request.protected_sources.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "volume sink requires protected sources"});
    }
    for (const auto& source : request.protected_sources) {
        if (source.empty()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "protected source path is empty"});
        }
        auto source_volume = path_volume(source);
        if (!source_volume) {
            return base::Result<void>::failure(source_volume.error());
        }
        if (equal_case_insensitive(request.path.native(), source_volume.value().native())) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "restore source is located on the target volume"});
        }
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_protected_sources_disk(const WindowsBlockSinkOpenRequest& request,
                                const std::uint32_t target_disk) {
    if (request.protected_sources.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "disk sink requires protected sources"});
    }
    for (const auto& source : request.protected_sources) {
        if (source.empty()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "protected source path is empty"});
        }
        auto source_disk = physical_disk_number_for_path(source);
        if (!source_disk) {
            return base::Result<void>::failure(source_disk.error());
        }
        if (source_disk.value() == target_disk) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "restore source is located on the target disk"});
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_request(const WindowsBlockSinkOpenRequest& request) {
    if (request.path.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "block sink path is empty"});
    }
    if (request.kind == WindowsBlockSinkKind::kVolume) {
        if (!WindowsBlockSink::is_canonical_volume_guid_path(request.path)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "target is not a canonical Volume GUID path"});
        }
        auto system = system_volume_path();
        if (!system) {
            return base::Result<void>::failure(system.error());
        }
        if (equal_case_insensitive(request.path.native(), system.value().native())) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "online system volume restore is forbidden"});
        }
        return validate_protected_sources_volume(request);
    }
    if (request.kind == WindowsBlockSinkKind::kPhysicalDisk) {
        if (!WindowsBlockSink::is_physical_drive_path(request.path)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "target is not a PhysicalDrive path"});
        }
        auto disk_number = WindowsBlockSink::physical_drive_number(request.path);
        if (!disk_number) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "PhysicalDrive number is invalid"});
        }
        auto system = is_system_physical_disk(disk_number.value());
        if (!system) {
            return base::Result<void>::failure(system.error());
        }
        if (system.value()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "online system disk restore is forbidden"});
        }
        return validate_protected_sources_disk(request, disk_number.value());
    }
    if (is_device_path(request.path.native())) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "stable file sink rejects device paths"});
    }
    return base::Result<void>::success();
}

std::filesystem::path open_path(const WindowsBlockSinkOpenRequest& request) {
    if (request.kind == WindowsBlockSinkKind::kVolume) {
        auto value = request.path.native();
        value.pop_back();
        return value;
    }
    return request.path;
}

base::Result<std::uint64_t> file_capacity(const HANDLE handle) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size) || size.QuadPart < 0) {
        return base::Result<std::uint64_t>::failure(
            detail::win32_error(GetLastError(), "GetFileSizeEx"));
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
}

base::Result<DWORD> device_control(const HANDLE handle, const DWORD control, void* output,
                                   const DWORD output_size, const char* operation) {
    detail::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<DWORD>::failure(detail::win32_error(GetLastError(), "CreateEventW"));
    }
    OVERLAPPED overlapped{};
    overlapped.hEvent = event.get();
    DWORD returned = 0;
    const auto started = DeviceIoControl(handle, control, nullptr, 0, output, output_size, nullptr,
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

base::Result<std::uint64_t> volume_capacity(const HANDLE handle) {
    GET_LENGTH_INFORMATION length{};
    auto result = device_control(handle, IOCTL_DISK_GET_LENGTH_INFO, &length, sizeof(length),
                                 "IOCTL_DISK_GET_LENGTH_INFO");
    if (!result || result.value() < sizeof(length) || length.Length.QuadPart <= 0) {
        return result ? base::Result<std::uint64_t>::failure(
                            {base::ErrorCode::kIoFailure, "volume length is invalid"})
                      : base::Result<std::uint64_t>::failure(result.error());
    }
    return base::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(length.Length.QuadPart));
}

base::Result<void> volume_control(const HANDLE handle, const DWORD control, const char* operation) {
    auto result = device_control(handle, control, nullptr, 0, operation);
    if (!result) {
        return base::Result<void>::failure(result.error());
    }
    return base::Result<void>::success();
}

void unlock_volume(const HANDLE handle) noexcept {
    try {
        static_cast<void>(volume_control(handle, FSCTL_UNLOCK_VOLUME, "FSCTL_UNLOCK_VOLUME"));
    } catch (...) {
    }
}

base::Result<void> wait_for_write(const HANDLE handle, OVERLAPPED& overlapped,
                                  const DWORD expected,
                                  const base::CancellationToken& cancellation) {
    constexpr DWORD kPollMilliseconds = 25;
    for (;;) {
        const auto wait = WaitForSingleObject(overlapped.hEvent, kPollMilliseconds);
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (wait != WAIT_TIMEOUT) {
            return base::Result<void>::failure(
                detail::win32_error(GetLastError(), "WaitForSingleObject"));
        }
        if (cancellation.stop_requested()) {
            CancelIoEx(handle, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "block write cancelled"});
        }
    }
    DWORD written = 0;
    if (!GetOverlappedResult(handle, &overlapped, &written, FALSE)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "GetOverlappedResult"));
    }
    if (written != expected) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "block write was incomplete"});
    }
    return base::Result<void>::success();
}

} // namespace

struct WindowsBlockSink::Impl final {
    detail::UniqueHandle handle;
    std::uint64_t capacity{0};
    bool volume_locked{false};

    ~Impl() {
        if (volume_locked && handle.valid()) {
            unlock_volume(handle.get());
        }
    }
};

WindowsBlockSink::WindowsBlockSink(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WindowsBlockSink::~WindowsBlockSink() = default;

base::Result<std::unique_ptr<WindowsBlockSink>>
WindowsBlockSink::open(const WindowsBlockSinkOpenRequest& request) {
    auto valid = validate_request(request);
    if (!valid) {
        return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(valid.error());
    }
    const auto share_mode = request.kind == WindowsBlockSinkKind::kStableFile
                                ? FILE_SHARE_READ
                                : (FILE_SHARE_READ | FILE_SHARE_WRITE);
    detail::UniqueHandle handle(CreateFileW(
        open_path(request).c_str(), GENERIC_READ | GENERIC_WRITE, share_mode, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED, nullptr));
    if (!handle.valid()) {
        return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(
            detail::win32_error(GetLastError(), "CreateFileW"));
    }
    bool locked = false;
    if (request.kind == WindowsBlockSinkKind::kVolume) {
        auto lock = volume_control(handle.get(), FSCTL_LOCK_VOLUME, "FSCTL_LOCK_VOLUME");
        if (!lock) {
            return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(lock.error());
        }
        locked = true;
        auto dismount = volume_control(handle.get(), FSCTL_DISMOUNT_VOLUME, "FSCTL_DISMOUNT_VOLUME");
        if (!dismount) {
            unlock_volume(handle.get());
            return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(dismount.error());
        }
    }
    auto capacity = (request.kind == WindowsBlockSinkKind::kVolume ||
                     request.kind == WindowsBlockSinkKind::kPhysicalDisk)
                        ? volume_capacity(handle.get())
                        : file_capacity(handle.get());
    if (!capacity) {
        return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(capacity.error());
    }
    if (request.expected_capacity_bytes && *request.expected_capacity_bytes != capacity.value()) {
        return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(
            {base::ErrorCode::kConflict, "block sink capacity changed"});
    }
    if (request.minimum_capacity_bytes && capacity.value() < *request.minimum_capacity_bytes) {
        return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(
            {base::ErrorCode::kInsufficientSpace, "block sink capacity is insufficient"});
    }
    if (request.kind == WindowsBlockSinkKind::kPhysicalDisk &&
        request.expected_bytes_per_sector != 0) {
        DISK_GEOMETRY geometry{};
        auto geometry_result =
            device_control(handle.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY, &geometry,
                           sizeof(geometry), "IOCTL_DISK_GET_DRIVE_GEOMETRY");
        if (!geometry_result || geometry_result.value() < sizeof(geometry) ||
            geometry.BytesPerSector != request.expected_bytes_per_sector) {
            return base::Result<std::unique_ptr<WindowsBlockSink>>::failure(
                {base::ErrorCode::kConflict, "target disk sector size does not match source"});
        }
    }
    auto impl = std::make_unique<Impl>();
    impl->handle = std::move(handle);
    impl->capacity = capacity.value();
    impl->volume_locked = locked;
    return base::Result<std::unique_ptr<WindowsBlockSink>>::success(
        std::unique_ptr<WindowsBlockSink>(new WindowsBlockSink(std::move(impl))));
}

bool WindowsBlockSink::is_canonical_volume_guid_path(
    const std::filesystem::path& path) noexcept {
    const auto value = std::wstring_view(path.native());
    return value.size() == kVolumePrefix.size() + 38 &&
           starts_with_case_insensitive(value, kVolumePrefix) && value[value.size() - 2] == L'}' &&
           value.back() == L'\\' &&
           valid_guid_body(value.substr(kVolumePrefix.size(), 36));
}

bool WindowsBlockSink::is_physical_drive_path(const std::filesystem::path& path) noexcept {
    return physical_drive_number(path).has_value();
}

std::optional<std::uint32_t>
WindowsBlockSink::physical_drive_number(const std::filesystem::path& path) noexcept {
    const auto value = std::wstring_view(path.native());
    if (!starts_with_case_insensitive(value, kPhysicalDrivePrefix)) {
        return std::nullopt;
    }
    const auto digits = value.substr(kPhysicalDrivePrefix.size());
    if (digits.empty() || digits.size() > 10) {
        return std::nullopt;
    }
    std::uint64_t number = 0;
    for (const wchar_t item : digits) {
        if (item < L'0' || item > L'9') {
            return std::nullopt;
        }
        number = number * 10U + static_cast<std::uint64_t>(item - L'0');
        if (number > (std::numeric_limits<std::uint32_t>::max)()) {
            return std::nullopt;
        }
    }
    return static_cast<std::uint32_t>(number);
}

std::uint64_t WindowsBlockSink::capacity_bytes() const noexcept { return impl_->capacity; }

base::Result<void> WindowsBlockSink::write(const std::uint64_t offset,
                                           const std::span<const std::byte> source,
                                           const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure({base::ErrorCode::kCancelled, "block write cancelled"});
    }
    if (offset > impl_->capacity || source.size() > impl_->capacity - offset ||
        source.size() > (std::numeric_limits<DWORD>::max)()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "block write range is invalid"});
    }
    if (source.empty()) {
        return base::Result<void>::success();
    }
    detail::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<void>::failure(detail::win32_error(GetLastError(), "CreateEventW"));
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();
    const auto size = static_cast<DWORD>(source.size());
    const auto started = WriteFile(impl_->handle.get(), source.data(), size, nullptr, &overlapped);
    const auto error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && error != ERROR_IO_PENDING) {
        return base::Result<void>::failure(detail::win32_error(error, "WriteFile"));
    }
    return wait_for_write(impl_->handle.get(), overlapped, size, cancellation);
}

base::Result<void> WindowsBlockSink::flush(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure({base::ErrorCode::kCancelled, "block flush cancelled"});
    }
    if (!FlushFileBuffers(impl_->handle.get())) {
        return base::Result<void>::failure(detail::win32_error(GetLastError(), "FlushFileBuffers"));
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_disk
