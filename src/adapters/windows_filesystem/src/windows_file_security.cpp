#include "windows_file_security.h"

#include "windows_file_handle.h"
#include "windows_file_names.h"

#include "aegra/base/error.h"

#include <aclapi.h>

#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {
namespace {

constexpr SECURITY_INFORMATION kFullSecurityInformation =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
    SACL_SECURITY_INFORMATION | PROTECTED_DACL_SECURITY_INFORMATION |
    PROTECTED_SACL_SECURITY_INFORMATION;

constexpr SECURITY_INFORMATION kReadSecurityInformation =
    OWNER_SECURITY_INFORMATION | GROUP_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION |
    SACL_SECURITY_INFORMATION;

[[nodiscard]] base::Result<void> enable_one_privilege(const wchar_t* privilege_name,
                                                      const std::string& operation) {
    HANDLE token = nullptr;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) ==
        FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "OpenProcessToken"));
    }
    UniqueHandle token_owner(token);
    LUID luid{};
    if (LookupPrivilegeValueW(nullptr, privilege_name, &luid) == FALSE) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "LookupPrivilegeValueW(" + operation + ")"));
    }
    TOKEN_PRIVILEGES privileges{};
    privileges.PrivilegeCount = 1;
    privileges.Privileges[0].Luid = luid;
    privileges.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    SetLastError(ERROR_SUCCESS);
    if (AdjustTokenPrivileges(token_owner.get(), FALSE, &privileges, sizeof(privileges), nullptr,
                              nullptr) == FALSE) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "AdjustTokenPrivileges(" + operation + ")"));
    }
    // AdjustTokenPrivileges can return TRUE with ERROR_NOT_ALL_ASSIGNED when the privilege
    // is missing from the token.
    const auto status = GetLastError();
    if (status != ERROR_SUCCESS) {
        return base::Result<void>::failure(
            win32_error(status, "AdjustTokenPrivileges(" + operation + ")"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Error security_unreadable(const DWORD error, std::string operation) {
    auto err = win32_error(error, std::move(operation));
    err.message.insert(0, "file_source.security_descriptor_unreadable: ");
    if (err.code != base::ErrorCode::kCancelled) {
        err.code = base::ErrorCode::kUnauthorized;
    }
    return err;
}

[[nodiscard]] base::Error security_unreadable(base::Error err) {
    err.message.insert(0, "file_source.security_descriptor_unreadable: ");
    if (err.code != base::ErrorCode::kCancelled) {
        err.code = base::ErrorCode::kUnauthorized;
    }
    return err;
}

} // namespace

base::Result<void> enable_file_backup_privileges() {
    auto backup = enable_one_privilege(L"SeBackupPrivilege", "SeBackupPrivilege");
    if (!backup) {
        return base::Result<void>::failure(security_unreadable(backup.error()));
    }
    auto security = enable_one_privilege(L"SeSecurityPrivilege", "SeSecurityPrivilege");
    if (!security) {
        return base::Result<void>::failure(security_unreadable(security.error()));
    }
    return base::Result<void>::success();
}

base::Result<void> enable_file_restore_privileges() {
    auto restore = enable_one_privilege(L"SeRestorePrivilege", "SeRestorePrivilege");
    if (!restore) {
        return restore;
    }
    return enable_one_privilege(L"SeSecurityPrivilege", "SeSecurityPrivilege");
}

base::Result<std::vector<std::byte>>
read_self_relative_security_descriptor(const std::wstring& absolute_path) {
    if (absolute_path.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.security_descriptor_unreadable"});
    }
    auto handle = open_path(to_utf16_vector(absolute_path), READ_CONTROL | ACCESS_SYSTEM_SECURITY,
                            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
                            FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (!handle) {
        return base::Result<std::vector<std::byte>>::failure(
            security_unreadable(handle.error()));
    }
    DWORD needed = 0;
    if (GetKernelObjectSecurity(handle.value().get(), kReadSecurityInformation, nullptr, 0,
                                &needed) != FALSE ||
        GetLastError() != ERROR_INSUFFICIENT_BUFFER || needed == 0) {
        return base::Result<std::vector<std::byte>>::failure(
            security_unreadable(GetLastError(), "GetKernelObjectSecurity(size)"));
    }
    if (needed > 64U * 1024U) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.metadata_limit"});
    }
    std::vector<std::byte> buffer(needed);
    DWORD written = 0;
    if (GetKernelObjectSecurity(handle.value().get(), kReadSecurityInformation,
                                reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data()), needed,
                                &written) == FALSE || written == 0 || written > needed) {
        return base::Result<std::vector<std::byte>>::failure(
            security_unreadable(GetLastError(), "GetKernelObjectSecurity"));
    }
    buffer.resize(written);
    auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(buffer.data());
    if (IsValidSecurityDescriptor(sd) == FALSE) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kUnauthorized, "file_source.security_descriptor_unreadable"});
    }
    // Require self-relative form for durable storage.
    SECURITY_DESCRIPTOR_CONTROL control = 0;
    DWORD revision = 0;
    if (GetSecurityDescriptorControl(sd, &control, &revision) == FALSE ||
        (control & SE_SELF_RELATIVE) == 0) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kUnauthorized, "file_source.security_descriptor_unreadable"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(buffer));
}

base::Result<void>
write_self_relative_security_descriptor(const std::wstring& absolute_path,
                                        const std::span<const std::byte> self_relative_sd) {
    if (absolute_path.empty() || self_relative_sd.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file_restore.security_descriptor_invalid"});
    }
    auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::byte*>(self_relative_sd.data()));
    if (IsValidSecurityDescriptor(sd) == FALSE) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "file_restore.security_descriptor_invalid"});
    }
    if (SetFileSecurityW(absolute_path.c_str(), kFullSecurityInformation, sd) == FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "SetFileSecurityW"));
    }
    return base::Result<void>::success();
}

base::Result<void>
write_self_relative_security_descriptor(const HANDLE handle,
                                        const std::span<const std::byte> self_relative_sd) {
    if (handle == nullptr || handle == INVALID_HANDLE_VALUE || self_relative_sd.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "file_restore.security_descriptor_invalid"});
    }
    auto* sd = reinterpret_cast<PSECURITY_DESCRIPTOR>(
        const_cast<std::byte*>(self_relative_sd.data()));
    if (IsValidSecurityDescriptor(sd) == FALSE) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCorruptData, "file_restore.security_descriptor_invalid"});
    }
    if (SetKernelObjectSecurity(handle, kFullSecurityInformation, sd) == FALSE) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "SetKernelObjectSecurity"));
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_filesystem::detail
