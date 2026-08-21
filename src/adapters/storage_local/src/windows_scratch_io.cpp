#include "windows_scratch_internal.h"

#include <winioctl.h>

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

namespace aegra::adapters::storage_local::detail {
namespace {

[[nodiscard]] wchar_t ascii_lower_w(const wchar_t value) noexcept {
    return value >= L'A' && value <= L'Z' ? static_cast<wchar_t>(value + (L'a' - L'A')) : value;
}

[[nodiscard]] bool is_unc_or_device_path(const std::wstring_view native) noexcept {
    if (native.starts_with(L"\\\\?\\UNC\\") || native.starts_with(L"\\\\.\\")) {
        return true;
    }
    return native.size() >= 2 && native[0] == L'\\' && native[1] == L'\\' &&
           !native.starts_with(L"\\\\?\\");
}

[[nodiscard]] std::wstring ensure_extended_local(std::wstring path) {
    if (path.starts_with(L"\\\\?\\")) {
        return path;
    }
    return L"\\\\?\\" + path;
}

[[nodiscard]] std::wstring strip_extended_prefix(std::wstring_view path) {
    if (path.starts_with(L"\\\\?\\") && !path.starts_with(L"\\\\?\\UNC\\")) {
        return std::wstring(path.substr(4));
    }
    return std::wstring(path);
}

[[nodiscard]] std::wstring ensure_trailing_slash(std::wstring value) {
    if (value.empty() || (value.back() != L'\\' && value.back() != L'/')) {
        value.push_back(L'\\');
    }
    return value;
}

} // namespace

std::uint32_t scratch_crc32_ieee(const std::span<const std::byte> data) noexcept {
    std::uint32_t crc = 0xFFFFFFFFU;
    for (const auto byte : data) {
        crc ^= static_cast<std::uint32_t>(std::to_integer<unsigned char>(byte));
        for (int bit = 0; bit < 8; ++bit) {
            const auto mask = static_cast<std::uint32_t>(-(static_cast<std::int32_t>(crc & 1U)));
            crc = (crc >> 1U) ^ (0xEDB88320U & mask);
        }
    }
    return ~crc;
}

base::Result<std::wstring> scratch_utf8_to_wide(const std::string_view utf8) {
    if (utf8.empty() || utf8.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path encoding is invalid"));
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                                          static_cast<int>(utf8.size()), nullptr, 0);
    if (size <= 0) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path encoding is invalid"));
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(),
                            static_cast<int>(utf8.size()), result.data(), size) != size) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path encoding is invalid"));
    }
    return base::Result<std::wstring>::success(std::move(result));
}

base::Result<std::string> scratch_wide_to_utf8(const std::wstring_view wide) {
    if (wide.empty() ||
        wide.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "scratch path encoding failed"));
    }
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "scratch path encoding failed"));
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, wide.data(),
                            static_cast<int>(wide.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "scratch path encoding failed"));
    }
    return base::Result<std::string>::success(std::move(result));
}

bool scratch_equals_ignore_case(const std::wstring_view left,
                                const std::wstring_view right) noexcept {
    if (left.size() != right.size()) {
        return false;
    }
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower_w(left[index]) != ascii_lower_w(right[index])) {
            return false;
        }
    }
    return true;
}

base::Result<std::wstring> scratch_resolve_absolute_local_path(const std::string_view path_utf8) {
    if (path_utf8.empty()) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path is empty"));
    }
    auto wide = scratch_utf8_to_wide(path_utf8);
    if (!wide) {
        return wide;
    }
    auto path = strip_extended_prefix(wide.value());
    if (is_unc_or_device_path(path) || path.starts_with(L"\\\\?\\Volume{")) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path must be local absolute"));
    }
    std::wstring full;
    const DWORD needed = GetFullPathNameW(path.c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return base::Result<std::wstring>::failure(
            win32_error(GetLastError(), "GetFullPathNameW scratch path"));
    }
    full.resize(static_cast<std::size_t>(needed));
    const DWORD written =
        GetFullPathNameW(path.c_str(), needed, full.data(), nullptr);
    if (written == 0 || written >= needed) {
        return base::Result<std::wstring>::failure(
            win32_error(GetLastError(), "GetFullPathNameW scratch path"));
    }
    full.resize(static_cast<std::size_t>(written));
    if (full.size() < 3 || full[1] != L':' || (full[2] != L'\\' && full[2] != L'/')) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path must be local absolute"));
    }
    if (is_unc_or_device_path(full)) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch path must be local absolute"));
    }
    return base::Result<std::wstring>::success(ensure_extended_local(std::move(full)));
}

base::Result<void> scratch_require_parent_directory(const std::wstring& path) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos || slash < 3) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch parent directory is missing"));
    }
    std::wstring parent = path.substr(0, slash);
    if (parent == L"\\\\?" || parent.size() <= 4) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch parent directory is missing"));
    }
    const auto attributes = GetFileAttributesW(parent.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kNotFound, "scratch parent directory does not exist"));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return base::Result<void>::failure(local_error(
            base::ErrorCode::kInvalidArgument, "scratch parent path is not a safe directory"));
    }
    return base::Result<void>::success();
}

base::Result<void>
scratch_reject_forbidden_volume(const std::wstring& absolute_path,
                                const std::string_view forbidden_volume_guid_utf8) {
    if (forbidden_volume_guid_utf8.empty()) {
        return base::Result<void>::success();
    }
    auto forbidden = scratch_utf8_to_wide(forbidden_volume_guid_utf8);
    if (!forbidden) {
        return base::Result<void>::failure(forbidden.error());
    }
    auto forbidden_norm = ensure_trailing_slash(std::move(forbidden).value());
    std::wstring query_path = strip_extended_prefix(absolute_path);
    wchar_t volume_path[MAX_PATH]{};
    if (!GetVolumePathNameW(query_path.c_str(), volume_path, MAX_PATH)) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "GetVolumePathNameW scratch"));
    }
    wchar_t volume_name[MAX_PATH]{};
    if (!GetVolumeNameForVolumeMountPointW(ensure_trailing_slash(volume_path).c_str(), volume_name,
                                           MAX_PATH)) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "GetVolumeNameForVolumeMountPointW scratch"));
    }
    if (scratch_equals_ignore_case(volume_name, forbidden_norm)) {
        return base::Result<void>::failure(local_error(
            base::ErrorCode::kConflict, "scratch path resides on forbidden restore target volume"));
    }
    return base::Result<void>::success();
}

base::Result<UniqueHandle> scratch_create_sparse_file(const std::wstring& path,
                                                     const std::uint64_t logical_size_bytes) {
    UniqueHandle handle(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!handle.valid()) {
        return base::Result<UniqueHandle>::failure(
            win32_error(GetLastError(), "CreateFileW scratch"));
    }
    const auto fail_created = [&](base::Error error) -> base::Result<UniqueHandle> {
        handle.reset();
        DeleteFileW(path.c_str());
        return base::Result<UniqueHandle>::failure(std::move(error));
    };
    DWORD bytes_returned = 0;
    if (!DeviceIoControl(handle.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes_returned,
                         nullptr)) {
        return fail_created(win32_error(GetLastError(), "FSCTL_SET_SPARSE scratch"));
    }
    if (logical_size_bytes > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return fail_created(
            local_error(base::ErrorCode::kInvalidArgument, "scratch logical size is too large"));
    }
    LARGE_INTEGER size{};
    size.QuadPart = static_cast<LONGLONG>(logical_size_bytes);
    if (!SetFilePointerEx(handle.get(), size, nullptr, FILE_BEGIN)) {
        return fail_created(win32_error(GetLastError(), "SetFilePointerEx scratch size"));
    }
    if (!SetEndOfFile(handle.get())) {
        return fail_created(win32_error(GetLastError(), "SetEndOfFile scratch"));
    }
    return base::Result<UniqueHandle>::success(std::move(handle));
}

base::Result<void> scratch_read_exact(const HANDLE handle, const std::uint64_t offset,
                                      const std::span<std::byte> destination) {
    if (destination.empty()) {
        return base::Result<void>::success();
    }
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch read offset is invalid"));
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        return base::Result<void>::failure(win32_error(GetLastError(), "seek scratch read"));
    }
    std::size_t done = 0;
    while (done < destination.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            destination.size() - done, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD got = 0;
        if (!ReadFile(handle, destination.data() + done, chunk, &got, nullptr)) {
            return base::Result<void>::failure(win32_error(GetLastError(), "ReadFile scratch"));
        }
        if (got == 0) {
            std::memset(destination.data() + done, 0, destination.size() - done);
            break;
        }
        done += got;
    }
    return base::Result<void>::success();
}

base::Result<void> scratch_write_exact(const HANDLE handle, const std::uint64_t offset,
                                       const std::span<const std::byte> source) {
    if (source.empty()) {
        return base::Result<void>::success();
    }
    if (offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch write offset is invalid"));
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle, position, nullptr, FILE_BEGIN)) {
        return base::Result<void>::failure(win32_error(GetLastError(), "seek scratch write"));
    }
    std::size_t done = 0;
    while (done < source.size()) {
        const auto chunk = static_cast<DWORD>((std::min)(
            source.size() - done, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD written = 0;
        if (!WriteFile(handle, source.data() + done, chunk, &written, nullptr)) {
            return base::Result<void>::failure(win32_error(GetLastError(), "WriteFile scratch"));
        }
        if (written != chunk) {
            return base::Result<void>::failure(
                local_error(base::ErrorCode::kIoFailure, "WriteFile scratch short write"));
        }
        done += written;
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::storage_local::detail
