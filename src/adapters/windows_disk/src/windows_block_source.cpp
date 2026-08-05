#include "aegra/adapters/windows_disk/windows_disk.h"

#include "windows_api.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <cwctype>
#include <limits>
#include <string>
#include <string_view>
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

base::Result<std::uint64_t> raw_volume_size(const HANDLE handle) {
    GET_LENGTH_INFORMATION length{};
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0, &length, sizeof(length),
                         &bytes_returned, nullptr)) {
        return base::Result<std::uint64_t>::failure(
            detail::win32_error(GetLastError(), "IOCTL_DISK_GET_LENGTH_INFO"));
    }
    if (bytes_returned < sizeof(length) || length.Length.QuadPart <= 0) {
        return base::Result<std::uint64_t>::failure(
            base::Error{base::ErrorCode::kIoFailure, "raw volume size is invalid"});
    }
    return base::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(length.Length.QuadPart));
}

// Prefer IOCTL length; fall back to GetFileSizeEx (same order as old DiskDevice::Open).
[[nodiscard]] std::uint64_t probe_device_length(const HANDLE handle) noexcept {
    if (auto length = raw_volume_size(handle)) {
        return length.value();
    }
    if (auto length = file_size(handle)) {
        return length.value();
    }
    return 0;
}

// Old BackupEngine: readable = device size; if 0 or > metadata, use metadata; trailing beyond
// readable is backed up as zero so the archive still spans the full logical volume length.
[[nodiscard]] std::uint64_t clamp_readable_size(const std::uint64_t logical_size,
                                                const std::uint64_t device_size) noexcept {
    if (device_size == 0 || device_size > logical_size) {
        return logical_size;
    }
    return device_size;
}

base::Result<void> enable_extended_raw_reads(const HANDLE handle) {
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle, FSCTL_ALLOW_EXTENDED_DASD_IO, nullptr, 0, nullptr, 0,
                         &bytes_returned, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "FSCTL_ALLOW_EXTENDED_DASD_IO"));
    }
    return base::Result<void>::success();
}

struct ResolvedDeviceSize final {
    std::uint64_t logical_size_bytes{0};
    std::uint64_t readable_size_bytes{0};
    bool zero_fill_unreadable{false};
};

base::Result<ResolvedDeviceSize> resolve_size(const WindowsBlockSourceOpenRequest& request,
                                              const HANDLE handle) {
    if (request.kind == WindowsBlockSourceKind::kVssSnapshot) {
        if (!request.expected_size_bytes || *request.expected_size_bytes == 0) {
            return base::Result<ResolvedDeviceSize>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "device block source requires a nonzero expected size",
            });
        }
        const auto logical = *request.expected_size_bytes;
        const auto readable = clamp_readable_size(logical, probe_device_length(handle));
        return base::Result<ResolvedDeviceSize>::success(
            ResolvedDeviceSize{logical, readable, true});
    }

    if (request.kind == WindowsBlockSourceKind::kRawVolume) {
        if (!request.expected_size_bytes || *request.expected_size_bytes == 0) {
            return base::Result<ResolvedDeviceSize>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "raw volume source requires a nonzero expected size",
            });
        }
        const auto logical = *request.expected_size_bytes;
        const auto readable = clamp_readable_size(logical, probe_device_length(handle));
        return base::Result<ResolvedDeviceSize>::success(
            ResolvedDeviceSize{logical, readable, true});
    }

    auto actual_size = file_size(handle);
    if (!actual_size) {
        return base::Result<ResolvedDeviceSize>::failure(actual_size.error());
    }
    if (request.expected_size_bytes && *request.expected_size_bytes != actual_size.value()) {
        return base::Result<ResolvedDeviceSize>::failure(base::Error{
            base::ErrorCode::kConflict,
            "stable file size does not match the expected size",
        });
    }
    return base::Result<ResolvedDeviceSize>::success(
        ResolvedDeviceSize{actual_size.value(), actual_size.value(), false});
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
    } else if (request.kind == WindowsBlockSourceKind::kRawVolume) {
        if (!WindowsBlockSource::is_canonical_volume_guid_path(request.path) ||
            !request.expected_size_bytes || *request.expected_size_bytes == 0) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "raw source requires a canonical Volume GUID path and nonzero expected size",
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

// Single asynchronous ReadFile. Windows volume/VSS drivers commonly return partial
// success (bytes_read < request) for large transfers — callers must loop.
base::Result<std::size_t> read_device_once(const HANDLE handle, const std::uint64_t offset,
                                          const std::span<std::byte> destination,
                                          const std::uint64_t request_size,
                                          const base::CancellationToken& cancellation) {
    if (request_size == 0) {
        return base::Result<std::size_t>::success(0);
    }
    detail::UniqueHandle event(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!event.valid()) {
        return base::Result<std::size_t>::failure(
            detail::win32_error(GetLastError(), "CreateEventW"));
    }

    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset & 0xFFFFFFFFULL);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();
    // Cap a single IRP; drivers often soft-cap transfer size (1–8 MiB). Loop fills the rest.
    constexpr std::uint64_t kMaxSingleRead = 1ULL * 1024ULL * 1024ULL;
    const auto once =
        (std::min)({request_size, kMaxSingleRead,
                    static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())});
    const auto started =
        ReadFile(handle, destination.data(), static_cast<DWORD>(once), nullptr, &overlapped);
    const auto start_error = started ? ERROR_SUCCESS : GetLastError();
    if (!started && start_error != ERROR_IO_PENDING) {
        if (start_error == ERROR_HANDLE_EOF) {
            return base::Result<std::size_t>::success(0);
        }
        return base::Result<std::size_t>::failure(detail::win32_error(start_error, "ReadFile"));
    }
    return wait_for_read(handle, overlapped, cancellation);
}

// Fill [offset, offset+request_size) or stop at true EOF (0-byte read).
base::Result<std::size_t> read_device_range(const HANDLE handle, const std::uint64_t offset,
                                           const std::span<std::byte> destination,
                                           const std::uint64_t request_size,
                                           const base::CancellationToken& cancellation) {
    if (request_size == 0) {
        return base::Result<std::size_t>::success(0);
    }
    if (destination.size() < static_cast<std::size_t>(request_size)) {
        return base::Result<std::size_t>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "block source read buffer is smaller than the request",
        });
    }
    std::size_t filled = 0;
    while (filled < static_cast<std::size_t>(request_size)) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                base::Error{base::ErrorCode::kCancelled, "block read cancelled"});
        }
        const auto remaining = request_size - static_cast<std::uint64_t>(filled);
        auto once = read_device_once(handle, offset + filled, destination.subspan(filled), remaining,
                                    cancellation);
        if (!once) {
            return once;
        }
        if (once.value() == 0) {
            break; // EOF
        }
        filled += once.value();
    }
    return base::Result<std::size_t>::success(filled);
}

void zero_fill(const std::span<std::byte> destination) noexcept {
    std::fill(destination.begin(), destination.end(), std::byte{0});
}

} // namespace

struct WindowsBlockSource::Impl final {
    detail::UniqueHandle handle;
    std::uint64_t size_bytes{0};
    std::uint64_t readable_size_bytes{0};
    bool zero_fill_unreadable{false};
};

WindowsBlockSource::WindowsBlockSource(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}

WindowsBlockSource::~WindowsBlockSource() = default;

base::Result<std::unique_ptr<WindowsBlockSource>>
WindowsBlockSource::open(const WindowsBlockSourceOpenRequest& request) {
    auto validation = validate_request(request);
    if (!validation) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(validation.error());
    }

    auto open_path = request.path.native();
    if (request.kind == WindowsBlockSourceKind::kRawVolume && !open_path.empty()) {
        open_path.pop_back();
    }
    const DWORD flags = request.kind == WindowsBlockSourceKind::kRawVolume
                            ? FILE_FLAG_OVERLAPPED
                            : FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED |
                                  FILE_FLAG_SEQUENTIAL_SCAN;
    detail::UniqueHandle handle(CreateFileW(
        open_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, flags, nullptr));
    if (!handle.valid()) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(
            detail::win32_error(GetLastError(), "CreateFileW"));
    }
    // Raw volumes and VSS snapshot devices both need extended DASD IO so reads past the
    // filesystem "end" (last clusters / trailing sectors) succeed like the old DiskDevice path.
    if (request.kind == WindowsBlockSourceKind::kRawVolume ||
        request.kind == WindowsBlockSourceKind::kVssSnapshot) {
        auto enabled = enable_extended_raw_reads(handle.get());
        if (!enabled) {
            // VSS devices sometimes reject the FSCTL; continue without it (probe/read may still
            // work for the full logical size). Raw volumes require it.
            if (request.kind == WindowsBlockSourceKind::kRawVolume) {
                return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(enabled.error());
            }
        }
    }

    auto size = resolve_size(request, handle.get());
    if (!size) {
        return base::Result<std::unique_ptr<WindowsBlockSource>>::failure(size.error());
    }

    auto impl = std::make_unique<Impl>();
    impl->handle = std::move(handle);
    impl->size_bytes = size.value().logical_size_bytes;
    impl->readable_size_bytes = size.value().readable_size_bytes;
    impl->zero_fill_unreadable = size.value().zero_fill_unreadable;
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

bool WindowsBlockSource::is_canonical_volume_guid_path(
    const std::filesystem::path& path) noexcept {
    return WindowsBlockSink::is_canonical_volume_guid_path(path);
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
    const auto out = destination.first(static_cast<std::size_t>(request_size));

    // Device/volume sources: logical size_bytes() stays inventory length. Only bytes past the
    // probed readable length are zero-filled (old trailing pad). Short/EOF inside the readable
    // range fails — never silent zero-fill of used data.
    if (impl_->zero_fill_unreadable) {
        if (offset >= impl_->readable_size_bytes) {
            zero_fill(out);
            return base::Result<std::size_t>::success(static_cast<std::size_t>(request_size));
        }
        const auto device_request =
            (std::min)(request_size, impl_->readable_size_bytes - offset);
        auto read_result =
            read_device_range(impl_->handle.get(), offset, out, device_request, cancellation);
        if (!read_result) {
            return read_result;
        }
        const auto bytes_read = read_result.value();
        // After looping partial IRPs, a short fill means true EOF inside the probed readable
        // range — treat as hard failure (do not silently zero used data).
        if (bytes_read < static_cast<std::size_t>(device_request)) {
            return base::Result<std::size_t>::failure(base::Error{
                base::ErrorCode::kIoFailure,
                "block source short read within readable device range (offset=" +
                    std::to_string(offset) + " requested=" + std::to_string(device_request) +
                    " got=" + std::to_string(bytes_read) +
                    " readable=" + std::to_string(impl_->readable_size_bytes) + ")",
            });
        }
        if (request_size > device_request) {
            zero_fill(out.subspan(static_cast<std::size_t>(device_request)));
        }
        return base::Result<std::size_t>::success(static_cast<std::size_t>(request_size));
    }

    return read_device_range(impl_->handle.get(), offset, out, request_size, cancellation);
}

} // namespace aegra::adapters::windows_disk
