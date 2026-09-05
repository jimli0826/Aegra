#include "virtualbox_paths.h"

// Windows trust headers require the base Windows types to be declared first.
// clang-format off
#include <windows.h>
#include <softpub.h>
#include <wintrust.h>
// clang-format on

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

namespace aegra::adapters::virtualbox::detail {
namespace {

constexpr const char* kProviderUnavailable = "bootcheck.provider_unavailable";
constexpr const char* kVmCreateFailed = "bootcheck.vm_create_failed";

base::Error path_error(const base::ErrorCode code, const char* message) {
    return base::Error{code, message};
}

base::Result<std::wstring> utf8_to_wide(const std::string_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    const int input_size = static_cast<int>(value.size());
    const int needed =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (needed <= 0) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, wide.data(),
                            needed) == 0) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    return base::Result<std::wstring>::success(std::move(wide));
}

base::Result<std::string> wide_to_utf8(const std::wstring_view value) {
    const int input_size = static_cast<int>(value.size());
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return base::Result<std::string>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, utf8.data(),
                            needed, nullptr, nullptr) == 0) {
        return base::Result<std::string>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    return base::Result<std::string>::success(std::move(utf8));
}

base::Result<std::wstring> normalize_local_path(const std::string_view path, const char* message) {
    auto wide = utf8_to_wide(path);
    if (!wide) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, message));
    }
    const DWORD needed = GetFullPathNameW(wide.value().c_str(), 0, nullptr, nullptr);
    if (needed == 0) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, message));
    }
    std::wstring normalized(needed, L'\0');
    const DWORD written =
        GetFullPathNameW(wide.value().c_str(), needed, normalized.data(), nullptr);
    if (written == 0 || written >= needed) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, message));
    }
    normalized.resize(written);
    if (normalized.size() < 4 || normalized[1] != L':' ||
        (normalized[2] != L'\\' && normalized[2] != L'/')) {
        return base::Result<std::wstring>::failure(
            path_error(base::ErrorCode::kInvalidArgument, message));
    }
    while (normalized.size() > 3 && normalized.back() == L'\\') {
        normalized.pop_back();
    }
    return base::Result<std::wstring>::success(std::move(normalized));
}

bool ensure_directory(const std::wstring& path) {
    DWORD attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(path.c_str(), nullptr)) {
            return false;
        }
        attributes = GetFileAttributesW(path.c_str());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    return true;
}

bool directory_is_empty(const std::wstring& path) {
    WIN32_FIND_DATAW entry{};
    HANDLE find = FindFirstFileW((path + L"\\*").c_str(), &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }
    bool empty = true;
    while (true) {
        if (wcscmp(entry.cFileName, L".") != 0 && wcscmp(entry.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
        if (!FindNextFileW(find, &entry)) {
            empty = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(find);
    return empty;
}

bool create_empty_directory(const std::wstring& path) {
    return ensure_directory(path) && directory_is_empty(path);
}

bool safe_job_id(const std::string_view job_id) {
    if (job_id.empty() || job_id.size() > 64) {
        return false;
    }
    return std::all_of(job_id.begin(), job_id.end(), [](const unsigned char value) {
        return std::isalnum(value) != 0 || value == '-';
    });
}

bool same_path(const std::wstring& left, const std::wstring& right) {
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

void assign_reason(std::string* reason, std::string value) {
    if (reason != nullptr) {
        *reason = std::move(value);
    }
}

LONG verify_signature_status(const std::wstring& path) {
    WINTRUST_FILE_INFO file{};
    file.cbStruct = sizeof(file);
    file.pcwszFilePath = path.c_str();
    WINTRUST_DATA trust{};
    trust.cbStruct = sizeof(trust);
    trust.dwUIChoice = WTD_UI_NONE;
    trust.fdwRevocationChecks = WTD_REVOKE_NONE;
    trust.dwUnionChoice = WTD_CHOICE_FILE;
    trust.pFile = &file;
    trust.dwStateAction = WTD_STATEACTION_VERIFY;
    trust.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
    GUID policy = WINTRUST_ACTION_GENERIC_VERIFY_V2;
    const LONG result = WinVerifyTrust(nullptr, &policy, &trust);
    trust.dwStateAction = WTD_STATEACTION_CLOSE;
    (void)WinVerifyTrust(nullptr, &policy, &trust);
    return result;
}

} // namespace

bool is_trusted_vbox_manage(const std::string_view executable_path, std::string* reason) {
    auto normalized = normalize_local_path(executable_path, kProviderUnavailable);
    if (!normalized) {
        assign_reason(reason, "path missing or not an absolute local path: \"" +
                                  std::string(executable_path) + "\"");
        return false;
    }
    const std::size_t leaf = normalized.value().find_last_of(L"\\/");
    if (leaf == std::wstring::npos ||
        _wcsicmp(normalized.value().c_str() + leaf + 1, L"VBoxManage.exe") != 0) {
        assign_reason(reason, "path does not name VBoxManage.exe");
        return false;
    }
    const DWORD attributes = GetFileAttributesW(normalized.value().c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        assign_reason(reason, "file not found or not a regular file");
        return false;
    }
    const LONG signature_status = verify_signature_status(normalized.value());
    if (signature_status != ERROR_SUCCESS) {
        char status[16]{};
        (void)std::snprintf(status, sizeof(status), "0x%08lX",
                            static_cast<unsigned long>(signature_status));
        assign_reason(reason,
                      std::string("Authenticode verification failed, WinVerifyTrust=") + status);
        return false;
    }
    return true;
}

base::Result<std::string> prepare_capability_user_home(const std::string_view requested_path) {
    auto path = normalize_local_path(requested_path, kProviderUnavailable);
    if (!path || !ensure_directory(path.value())) {
        return base::Result<std::string>::failure(
            path_error(base::ErrorCode::kIoFailure, kProviderUnavailable));
    }
    auto utf8 = wide_to_utf8(path.value());
    if (!utf8) {
        return base::Result<std::string>::failure(utf8.error());
    }
    return utf8;
}

base::Result<VirtualBoxJobLayout>
prepare_job_layout(const virtualization::BootCheckVmRequest& request, const bool use_isolated_home) {
    if (!safe_job_id(request.job_id) || request.cpu_count == 0 || request.cpu_count > 32 ||
        request.memory_mib < 1024 || request.memory_mib > 32768 ||
        request.overlay_limit_bytes == 0) {
        return base::Result<VirtualBoxJobLayout>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    auto root = normalize_local_path(request.job_directory, kVmCreateFailed);
    auto parent = normalize_local_path(request.parent_disk_path, kVmCreateFailed);
    if (!root || !parent) {
        return base::Result<VirtualBoxJobLayout>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    const std::wstring expected_parent = root.value() + L"\\present\\base.vmdk";
    const DWORD root_attributes = GetFileAttributesW(root.value().c_str());
    const DWORD parent_attributes = GetFileAttributesW(parent.value().c_str());
    if (!same_path(parent.value(), expected_parent) ||
        (root_attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (root_attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0 ||
        parent_attributes == INVALID_FILE_ATTRIBUTES ||
        (parent_attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return base::Result<VirtualBoxJobLayout>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }

    const std::wstring home = root.value() + L"\\vbox-home";
    const std::wstring vm_base = root.value() + L"\\vm";
    const std::wstring child = root.value() + L"\\child.vdi";
    // The isolated registry (LocalSystem path) needs its own vbox-home; the
    // user-visible path leaves user_home empty so VBoxManage falls back to the
    // invoking user's default registry.
    if ((use_isolated_home && !create_empty_directory(home)) || !create_empty_directory(vm_base) ||
        GetFileAttributesW(child.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return base::Result<VirtualBoxJobLayout>::failure(
            path_error(base::ErrorCode::kConflict, kVmCreateFailed));
    }

    auto root_utf8 = wide_to_utf8(root.value());
    auto parent_utf8 = wide_to_utf8(parent.value());
    auto home_utf8 = use_isolated_home ? wide_to_utf8(home)
                                       : base::Result<std::string>::success(std::string{});
    auto vm_base_utf8 = wide_to_utf8(vm_base);
    auto child_utf8 = wide_to_utf8(child);
    if (!root_utf8 || !parent_utf8 || !home_utf8 || !vm_base_utf8 || !child_utf8) {
        return base::Result<VirtualBoxJobLayout>::failure(
            path_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    VirtualBoxJobLayout layout;
    layout.job_root = std::move(root_utf8.value());
    layout.parent_vmdk = std::move(parent_utf8.value());
    layout.user_home = std::move(home_utf8.value());
    layout.vm_base_directory = std::move(vm_base_utf8.value());
    layout.child_medium = std::move(child_utf8.value());
    layout.vm_name = "Aegra-BootCheck-" + request.job_id;
    return base::Result<VirtualBoxJobLayout>::success(std::move(layout));
}

} // namespace aegra::adapters::virtualbox::detail
