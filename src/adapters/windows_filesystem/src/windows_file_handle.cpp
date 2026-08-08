#include "windows_file_handle.h"

#include <string>

namespace aegra::adapters::windows_filesystem::detail {

base::Error win32_error(const DWORD error, std::string operation) {
    base::ErrorCode code = base::ErrorCode::kIoFailure;
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        code = base::ErrorCode::kNotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        code = base::ErrorCode::kUnauthorized;
        break;
    case ERROR_OPERATION_ABORTED:
        code = base::ErrorCode::kCancelled;
        break;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        code = base::ErrorCode::kInsufficientSpace;
        break;
    case ERROR_ALREADY_EXISTS:
    case ERROR_FILE_EXISTS:
        code = base::ErrorCode::kConflict;
        break;
    default:
        break;
    }
    operation += " failed with Win32 error ";
    operation += std::to_string(error);
    return base::Error{code, std::move(operation)};
}

base::Result<UniqueHandle> open_path(const std::vector<std::uint16_t>& utf16_path,
                                     const DWORD access, const DWORD share,
                                     const DWORD disposition, const DWORD flags) {
    if (utf16_path.empty()) {
        return base::Result<UniqueHandle>::failure(
            {base::ErrorCode::kInvalidArgument, "filesystem path is empty"});
    }
    // wstring is already NUL-terminated via c_str(); do not embed an extra L'\0' character
    // (that would truncate the path for some Win32 callers).
    std::wstring path(utf16_path.begin(), utf16_path.end());
    while (!path.empty() && path.back() == L'\0') {
        path.pop_back();
    }
    if (path.empty()) {
        return base::Result<UniqueHandle>::failure(
            {base::ErrorCode::kInvalidArgument, "filesystem path is empty"});
    }
    UniqueHandle handle(CreateFileW(path.c_str(), access, share, nullptr, disposition, flags,
                                    nullptr));
    if (!handle.valid()) {
        return base::Result<UniqueHandle>::failure(win32_error(GetLastError(), "CreateFileW"));
    }
    return base::Result<UniqueHandle>::success(std::move(handle));
}

std::vector<std::uint16_t>
ensure_trailing_directory_separator(std::vector<std::uint16_t> utf16_path) {
    while (!utf16_path.empty() && utf16_path.back() == 0) {
        utf16_path.pop_back();
    }
    if (utf16_path.empty()) {
        return utf16_path;
    }
    const auto last = utf16_path.back();
    if (last != L'\\' && last != L'/') {
        utf16_path.push_back(static_cast<std::uint16_t>(L'\\'));
    }
    return utf16_path;
}

namespace {

[[nodiscard]] base::Result<void> check_ntfs_or_refs_name(const wchar_t* fs_name) {
    if (fs_name == nullptr || fs_name[0] == L'\0') {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.unsupported_filesystem"});
    }
    if (_wcsicmp(fs_name, L"NTFS") != 0 && _wcsicmp(fs_name, L"ReFS") != 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.unsupported_filesystem"});
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> ensure_ntfs_or_refs(const UniqueHandle& root) {
    if (!root.valid()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "filesystem root handle is invalid"});
    }
    wchar_t fs_name[MAX_PATH + 1]{};
    if (GetVolumeInformationByHandleW(root.get(), nullptr, 0, nullptr, nullptr, nullptr, fs_name,
                                      MAX_PATH) == FALSE) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "GetVolumeInformationByHandleW"));
    }
    return check_ntfs_or_refs_name(fs_name);
}

base::Result<void> ensure_ntfs_or_refs_path(const std::wstring& directory_root) {
    if (directory_root.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "filesystem root path is empty"});
    }
    std::wstring path = directory_root;
    while (!path.empty() && path.back() == L'\0') {
        path.pop_back();
    }
    if (!path.empty() && path.back() != L'\\' && path.back() != L'/') {
        path.push_back(L'\\');
    }
    wchar_t fs_name[MAX_PATH + 1]{};
    if (GetVolumeInformationW(path.c_str(), nullptr, 0, nullptr, nullptr, nullptr, fs_name,
                              MAX_PATH) == FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "GetVolumeInformationW"));
    }
    return check_ntfs_or_refs_name(fs_name);
}

} // namespace aegra::adapters::windows_filesystem::detail
