#include "local_storage_internal.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <string>
#include <string_view>

namespace aegra::adapters::storage_local::detail {
namespace {

constexpr auto kInternalPrefix = ".aegra-internal";
constexpr std::wstring_view kInternalPrefixWide = L".aegra-internal";

template <typename Character>
[[nodiscard]] constexpr Character ascii_lower(const Character value) noexcept {
    return value >= static_cast<Character>('A') && value <= static_cast<Character>('Z')
               ? static_cast<Character>(value + static_cast<Character>('a' - 'A'))
               : value;
}

[[nodiscard]] bool has_internal_prefix(const std::string_view value) noexcept {
    const std::string_view internal = kInternalPrefix;
    if (value.size() < internal.size()) {
        return false;
    }
    for (std::size_t index = 0; index < internal.size(); ++index) {
        if (ascii_lower(value[index]) != internal[index]) {
            return false;
        }
    }
    return value.size() == internal.size() || value[internal.size()] == '/';
}

[[nodiscard]] std::filesystem::path extended_path(const std::filesystem::path& path) {
    const auto native = path.native();
    if (native.starts_with(L"\\\\?\\")) {
        return path;
    }
    return std::filesystem::path(std::wstring(L"\\\\?\\") + native);
}

[[nodiscard]] base::Result<std::wstring> utf8_to_wide(const std::string_view encoded) {
    if (encoded.empty() || encoded.size() > (std::numeric_limits<int>::max)()) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "object key encoding is invalid"));
    }
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(),
                                          static_cast<int>(encoded.size()), nullptr, 0);
    if (size <= 0) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "object key encoding is invalid"));
    }
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, encoded.data(),
                            static_cast<int>(encoded.size()), result.data(), size) != size) {
        return base::Result<std::wstring>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "object key encoding is invalid"));
    }
    return base::Result<std::wstring>::success(std::move(result));
}

[[nodiscard]] base::Result<std::string> wide_to_utf8(const std::wstring_view value) {
    if (value.empty() || value.size() > (std::numeric_limits<int>::max)()) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "local object name encoding failed"));
    }
    const auto size =
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "local object name encoding failed"));
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                            static_cast<int>(value.size()), result.data(), size, nullptr,
                            nullptr) != size) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kIoFailure, "local object name encoding failed"));
    }
    return base::Result<std::string>::success(std::move(result));
}

[[nodiscard]] bool is_reparse_or_not_directory(const DWORD attributes) noexcept {
    return (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

[[nodiscard]] base::Result<void> inspect_directory(const std::filesystem::path& path,
                                                   const bool create_missing) {
    auto attributes = GetFileAttributesW(path.c_str());
    const auto inspection_error = GetLastError();
    if (attributes == INVALID_FILE_ATTRIBUTES && create_missing &&
        (inspection_error == ERROR_FILE_NOT_FOUND || inspection_error == ERROR_PATH_NOT_FOUND)) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            const auto create_error = GetLastError();
            if (create_error != ERROR_ALREADY_EXISTS) {
                return base::Result<void>::failure(
                    win32_error(create_error, "create local storage directory"));
            }
        }
        attributes = GetFileAttributesW(path.c_str());
    }
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "inspect local storage directory"));
    }
    if (is_reparse_or_not_directory(attributes)) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "local storage path is not a safe directory"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> check_root_directory_chain(const std::filesystem::path& root,
                                                            const bool create_missing) {
    auto current = root.root_path();
    if (current.empty()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "local storage root is invalid"));
    }
    auto inspected = inspect_directory(extended_path(current), false);
    if (!inspected) {
        return inspected;
    }
    for (const auto& component : root.relative_path()) {
        if (component == L".") {
            continue;
        }
        if (component == L"..") {
            return base::Result<void>::failure(
                local_error(base::ErrorCode::kInvalidArgument, "local storage root is invalid"));
        }
        current /= component;
        inspected = inspect_directory(extended_path(current), create_missing);
        if (!inspected) {
            return inspected;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> check_directory_chain(const LocalObjectStorageState& state,
                                                       const std::filesystem::path& path,
                                                       const bool create_missing) {
    const auto parent = path.parent_path();
    const auto relative = parent.lexically_relative(state.root);
    if (relative.empty() && parent != state.root) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "object path escapes storage root"));
    }
    auto current = state.root;
    for (const auto& component : relative) {
        if (component == L".") {
            continue;
        }
        if (component == L"..") {
            return base::Result<void>::failure(
                local_error(base::ErrorCode::kInvalidArgument, "object path escapes storage root"));
        }
        current /= component;
        auto inspected = inspect_directory(current, create_missing);
        if (!inspected) {
            return inspected;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::string generation(const BY_HANDLE_FILE_INFORMATION& information) {
    std::string result;
    result.reserve(96);
    const std::uint64_t size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) | information.nFileSizeLow;
    result.append(std::to_string(information.dwVolumeSerialNumber));
    result.append("-").append(std::to_string(information.nFileIndexHigh));
    result.append("-").append(std::to_string(information.nFileIndexLow));
    result.append("-").append(std::to_string(information.ftLastWriteTime.dwHighDateTime));
    result.append("-").append(std::to_string(information.ftLastWriteTime.dwLowDateTime));
    result.append("-").append(std::to_string(size));
    return result;
}

[[nodiscard]] bool is_unc_path(const std::filesystem::path& path) noexcept {
    const auto& native = path.native();
    return native.size() >= 2 && native[0] == L'\\' && native[1] == L'\\';
}

[[nodiscard]] base::Result<std::filesystem::path>
prepare_local_root(const LocalObjectStorageOpenRequest& request) {
    std::error_code filesystem_error;
    auto absolute = std::filesystem::absolute(request.root, filesystem_error).lexically_normal();
    if (filesystem_error || is_unc_path(absolute)) {
        return base::Result<std::filesystem::path>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "local storage root is invalid"));
    }
    const auto create_missing = request.mode == LocalRootMode::kCreateIfMissing;
    auto safe = check_root_directory_chain(absolute, create_missing);
    if (!safe) {
        return base::Result<std::filesystem::path>::failure(safe.error());
    }
    auto canonical = std::filesystem::weakly_canonical(absolute, filesystem_error);
    if (filesystem_error) {
        return base::Result<std::filesystem::path>::failure(
            local_error(base::ErrorCode::kIoFailure, "canonicalize local storage root failed"));
    }
    safe = check_root_directory_chain(canonical, false);
    return safe ? base::Result<std::filesystem::path>::success(extended_path(canonical))
                : base::Result<std::filesystem::path>::failure(safe.error());
}

} // namespace

base::Error local_error(const base::ErrorCode code, const char* message) { return {code, message}; }

base::Error win32_error(const DWORD code, const char* operation) {
    auto error_code = base::ErrorCode::kIoFailure;
    if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
        error_code = base::ErrorCode::kNotFound;
    } else if (code == ERROR_FILE_EXISTS || code == ERROR_ALREADY_EXISTS ||
               code == ERROR_SHARING_VIOLATION || code == ERROR_LOCK_VIOLATION) {
        error_code = base::ErrorCode::kConflict;
    } else if (code == ERROR_ACCESS_DENIED || code == ERROR_PRIVILEGE_NOT_HELD) {
        error_code = base::ErrorCode::kUnauthorized;
    } else if (code == ERROR_DISK_FULL || code == ERROR_HANDLE_DISK_FULL) {
        error_code = base::ErrorCode::kInsufficientSpace;
    }
    return {error_code,
            std::string(operation) + " failed with Win32 error " + std::to_string(code)};
}

base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kCancelled, "local storage operation cancelled"));
    }
    return base::Result<void>::success();
}

bool is_local_object_key(const std::string_view key) noexcept {
    return ports::is_valid_object_key(key) && !has_internal_prefix(key);
}

bool is_local_object_prefix(const std::string_view prefix) noexcept {
    return ports::is_valid_object_prefix(prefix) && !has_internal_prefix(prefix);
}

bool is_internal_component(const std::wstring_view component) noexcept {
    if (component.size() != kInternalPrefixWide.size()) {
        return false;
    }
    for (std::size_t index = 0; index < component.size(); ++index) {
        if (ascii_lower(component[index]) != kInternalPrefixWide[index]) {
            return false;
        }
    }
    return true;
}

base::Result<std::filesystem::path> object_path(const LocalObjectStorageState& state,
                                                const std::string_view key) {
    if (!is_local_object_key(key)) {
        return base::Result<std::filesystem::path>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "local object key is invalid"));
    }
    auto decoded = utf8_to_wide(key);
    if (!decoded) {
        return base::Result<std::filesystem::path>::failure(decoded.error());
    }
    auto relative = std::filesystem::path(std::move(decoded).value());
    relative.make_preferred();
    return base::Result<std::filesystem::path>::success(state.root / relative);
}

base::Result<std::filesystem::path> internal_write_path(const LocalObjectStorageState& state,
                                                        const std::string_view key) {
    auto decoded = utf8_to_wide(key);
    if (!decoded) {
        return base::Result<std::filesystem::path>::failure(decoded.error());
    }
    auto relative = std::filesystem::path(std::move(decoded).value());
    relative.make_preferred();
    return base::Result<std::filesystem::path>::success(state.root / L".aegra-internal" /
                                                        L"writes" / relative);
}

base::Result<std::string> object_key(const LocalObjectStorageState& state,
                                     const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(state.root);
    if (relative.empty() || is_internal_component(relative.begin()->native())) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "local object path is internal"));
    }
    auto encoded = wide_to_utf8(relative.generic_wstring());
    if (!encoded || !is_local_object_key(encoded.value())) {
        return base::Result<std::string>::failure(
            local_error(base::ErrorCode::kCorruptData, "local object name is invalid"));
    }
    return encoded;
}

base::Result<void> ensure_safe_parent_directories(const LocalObjectStorageState& state,
                                                  const std::filesystem::path& path) {
    return check_directory_chain(state, path, true);
}

base::Result<void> validate_safe_parent_directories(const LocalObjectStorageState& state,
                                                    const std::filesystem::path& path) {
    return check_directory_chain(state, path, false);
}

base::Result<UniqueHandle> open_regular_file(const LocalObjectStorageState& state,
                                             const std::filesystem::path& path,
                                             const DWORD desired_access) {
    auto parents = validate_safe_parent_directories(state, path);
    if (!parents) {
        return base::Result<UniqueHandle>::failure(parents.error());
    }
    UniqueHandle handle(CreateFileW(path.c_str(), desired_access | FILE_READ_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                                    OPEN_EXISTING,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    if (!handle.valid()) {
        return base::Result<UniqueHandle>::failure(
            win32_error(GetLastError(), "open local object"));
    }
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle.get(), &information)) {
        return base::Result<UniqueHandle>::failure(
            win32_error(GetLastError(), "inspect local object"));
    }
    if ((information.dwFileAttributes &
         (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return base::Result<UniqueHandle>::failure(
            local_error(base::ErrorCode::kConflict, "local object is not a regular file"));
    }
    return base::Result<UniqueHandle>::success(std::move(handle));
}

base::Result<ports::ObjectAttributes> attributes_from_handle(const std::string_view key,
                                                             const HANDLE handle) {
    BY_HANDLE_FILE_INFORMATION information{};
    if (!GetFileInformationByHandle(handle, &information)) {
        return base::Result<ports::ObjectAttributes>::failure(
            win32_error(GetLastError(), "read local object attributes"));
    }
    const std::uint64_t size =
        (static_cast<std::uint64_t>(information.nFileSizeHigh) << 32U) | information.nFileSizeLow;
    return base::Result<ports::ObjectAttributes>::success(
        {std::string(key), size, generation(information)});
}

base::Result<ports::ObjectAttributes> attributes_for_path(const LocalObjectStorageState& state,
                                                          const std::string_view key,
                                                          const std::filesystem::path& path) {
    auto handle = open_regular_file(state, path, 0);
    if (!handle) {
        return base::Result<ports::ObjectAttributes>::failure(handle.error());
    }
    return attributes_from_handle(key, handle.value().get());
}

base::Result<std::shared_ptr<LocalObjectStorageState>>
open_local_state(const LocalObjectStorageOpenRequest& request) {
    if (request.root.empty() || request.maximum_read_size == 0 ||
        (request.mode != LocalRootMode::kOpenExisting &&
         request.mode != LocalRootMode::kCreateIfMissing)) {
        return base::Result<std::shared_ptr<LocalObjectStorageState>>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "local storage request is invalid"));
    }
    auto root = prepare_local_root(request);
    if (!root) {
        return base::Result<std::shared_ptr<LocalObjectStorageState>>::failure(root.error());
    }
    auto state = std::make_shared<LocalObjectStorageState>();
    state->root = std::move(root).value();
    state->maximum_read_size = request.maximum_read_size;
    return base::Result<std::shared_ptr<LocalObjectStorageState>>::success(std::move(state));
}

} // namespace aegra::adapters::storage_local::detail
