#include "aegra/adapters/virtualbox/virtualbox_discovery.h"

#include <windows.h>

#include <limits>
#include <string_view>

namespace aegra::adapters::virtualbox {
namespace {

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty() ||
        value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return {};
    }
    const int input_size = static_cast<int>(value.size());
    const int needed = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size,
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, utf8.data(),
                            needed, nullptr, nullptr) == 0) {
        return {};
    }
    return utf8;
}

[[nodiscard]] std::wstring registry_install_directory() {
    DWORD size = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oracle\\VirtualBox", L"InstallDir",
                     RRF_RT_REG_SZ, nullptr, nullptr, &size) != ERROR_SUCCESS ||
        size < sizeof(wchar_t)) {
        return {};
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Oracle\\VirtualBox", L"InstallDir",
                     RRF_RT_REG_SZ, nullptr, value.data(), &size) != ERROR_SUCCESS) {
        return {};
    }
    while (!value.empty() && (value.back() == L'\0' || value.back() == L'\\')) {
        value.pop_back();
    }
    return value;
}

[[nodiscard]] std::wstring program_files_install_directory() {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD written =
        GetEnvironmentVariableW(L"ProgramFiles", buffer.data(), static_cast<DWORD>(buffer.size()));
    if (written == 0 || written >= buffer.size()) {
        return {};
    }
    buffer.resize(written);
    return buffer + L"\\Oracle\\VirtualBox";
}

[[nodiscard]] bool is_existing_file(const std::wstring& path) {
    const DWORD attributes = GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == 0;
}

} // namespace

std::string discover_vbox_manage_path() {
    for (const auto& directory :
         {registry_install_directory(), program_files_install_directory()}) {
        if (directory.empty()) {
            continue;
        }
        const std::wstring candidate = directory + L"\\VBoxManage.exe";
        if (is_existing_file(candidate)) {
            return wide_to_utf8(candidate);
        }
    }
    return {};
}

} // namespace aegra::adapters::virtualbox
