#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <cwctype>
#include <limits>
#include <string_view>
#include <thread>
#include <utility>

namespace aegra::adapters::windows_disk {
namespace {

constexpr std::wstring_view kVssPathPrefix = LR"(\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopy)";
constexpr std::wstring_view kWin32DevicePrefix = LR"(\\.\)";
constexpr std::wstring_view kGlobalRootDevicePrefix = LR"(\\?\GLOBALROOT\Device\)";

bool starts_with_case_insensitive(const std::wstring_view value,
                                  const std::wstring_view prefix) noexcept {
    if (value.size() < prefix.size()) {
        return false;
    }
    return std::ranges::equal(value.substr(0, prefix.size()), prefix,
                              [](const wchar_t left, const wchar_t right) {
                                  return std::towlower(left) == std::towlower(right);
                              });
}

bool is_device_namespace_path(const std::wstring_view path) noexcept {
    return starts_with_case_insensitive(path, kWin32DevicePrefix) ||
           starts_with_case_insensitive(path, kGlobalRootDevicePrefix);
}

base::Result<std::uint64_t> file_size(const HANDLE handle) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(handle, &size)) {
        return base::Result<std::uint64_t>::failure(
            detail::win32_error(GetLastError(), "GetFileSizeEx"));
    }
    if (size.QuadPart < 0) {
        return base::Result<std::uint64_t>::failure(
            base::Error{base::ErrorCode::kIoFailure, "file size is negative"});
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(size.QuadPart));
}

base::Result<std::uint64_t> resolve_size(const WindowsBlockSourceOpenRequest& request,
                                         const HANDLE handle) {
    if (request.kind == WindowsBlockSourceKind::kVssSnapshot) {
        if (!request.expected_size_bytes || *request.expected_size_bytes == 0) {
            return base::Result<std::uint64_t>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "VSS snapshot source requires a nonzero expected size",
            });
        }
        return base::Result<std::uint64_t>::success(*request.expected_size_bytes);
    }

    auto actual_size = file_size(handle);
    if (!actual_size) {
        return actual_size;
    }
    if (request.expected_size_bytes && *request.expected_size_bytes != actual_size.value()) {
        return base::Result<std::uint64_t>::failure(base::Error{
            base::ErrorCode::kConflict,
            "stable file size does not match the expected size",
        });
    }
    return actual_size;
}

base::Result<void> validate_request(const WindowsBlockSourceOpenRequest& request) {
    if (request.path.empty()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "block source path is empty"});
    }
    const auto path = std::wstring_view(request.path.native());
    if (request.kind == WindowsBlockSourceKind::kVssSnapshot) {
        if (!WindowsBlockSource::is_vss_snapshot_device_path(request.path)) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "VSS snapshot path is not a canonical snapshot device object",
            });
        }
        if (!request.expected_size_bytes || *request.expected_size_bytes == 0) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "VSS snapshot source requires a nonzero expected size",
            });
        }
    } else if (is_device_namespace_path(path)) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "stable file mode does not accept Windows device namespace paths",
        });
    }
    return base::Result<void>::success();
}

base::Result<std::size_t> wait_for_read(const HANDLE handle, OVERLAPPED& overlapped,
                                        const base::CancellationToken& cancellation) {
    constexpr DWORD kPollIntervalMilliseconds = 25;
    for (;;) {
        const auto wait_result = WaitForSingleObject(overlapped.hEvent, kPollIntervalMilliseconds);
        if (wait_result == WAIT_OBJECT_0) {
            break;
        }
        if (wait_result != WAIT_TIMEOUT) {
            return base::Result<std::size_t>::failure(
                detail::win32_error(GetLastError(), "WaitForSingleObject"));
        }
        if (cancellation.stop_requested()) {
            CancelIoEx(handle, &overlapped);
            WaitForSingleObject(overlapped.hEvent, INFINITE);
            return base::Result<std::size_t>::failure(
                base::Error{base::ErrorCode::kCancelled, "block read cancelled"});
        }
    }

    DWORD bytes_read = 0;
    if (!GetOverlappedResult(handle, &overlapped, &bytes_read, FALSE)) {
        const auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF) {
            return base::Result<std::size_t>::success(0);
        }
        return base::Result<std::size_t>::failure(
            detail::win32_error(error, "GetOverlappedResult"));
    }
    return base::Result<std::size_t>::success(static_cast<std::size_t>(bytes_read));
}

} // namespace

struct WindowsBlockSource::Impl final {
    detail::UniqueHandle handle;
    std::uint64_t size_bytes{0};
};

WindowsBlockSource::WindowsBlockSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WindowsBlockSource::~WindowsBlockSource() = default;

base::Result<std::unique_ptr<WindowsBlockSource>>
WindowsBlockSource::open(const WindowsBlockSourceOpenRequest& request) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(validation.error());
    }

    detail::UniqueHandle handle(CreateFileW(
        request.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(
            detail::win32_error(GetLastError(), "CreateFileW"));
    }

    auto size = resolve_size(request, handle.get());
    if (!size) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(size.error());
    }

    auto impl = std::make_unique<Impl>();
    impl->handle = std::move(handle);
    impl->size_bytes = size.value();
    return base::Result<std::unique_ptr<WindowsBlockSource>>::success(
        std::unique_ptr<WindowsBlockSource>(new WindowsBlockSource(std::move(impl))));
}

bool WindowsBlockSource::is_vss_snapshot_device_path(const std::filesystem::path& path) noexcept {
    const auto value = std::wstring_view(path.native());
    if (!starts_with_case_insensitive(value, kVssPathPrefix) ||
        value.size() == kVssPathPrefix.size()) {
        return false;
    }
    return std::ranges::all_of(value.substr(kVssPathPrefix.size()), [](const wchar_t character) {
        return character >= L'0' && character <= L'9';
    });
}

std::uint64_t WindowsBlockSource::size_bytes() const noexcept { return impl_->size_bytes; }

base::Result<std::size_t> WindowsBlockSource::read(const std::uint64_t offset,
                                                   const std::span<std::byte> destination,
                                                   const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kCancelled, "block read cancelled"});
    }
    if (offset > impl_->size_bytes) {
        return base::Result<std::size_t>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "read offset is out of range"});
    }
    if (destination.empty() || offset == impl_->size_bytes) {
        return base::Result<std::size_t>::success(0);
    }

    const auto remaining = impl_->size_bytes - offset;
    const auto request_size =
        (std::min)({static_cast<std::uint64_t>(destination.size()), remaining,
                    static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())});
    detail::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<std::size_t>::failure(
            detail::win32_error(GetLastError(), "CreateEventW"));
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();
    const auto started = ReadFile(impl_->handle.get(), destination.data(),
                                  static_cast<DWORD>(request_size), nullptr, &overlapped);
    const auto start_error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && start_error != ERROR_IO_PENDING) {
        const auto error = start_error;
        if (error == ERROR_HANDLE_EOF) {
            return base::Result<std::size_t>::success(0);
        }
        return base::Result<std::size_t>::failure(detail::win32_error(error, "ReadFile"));
    }
    return wait_for_read(impl_->handle.get(), overlapped, cancellation);
}

} // namespace aegra::adapters::windows_disk
