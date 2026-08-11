#include "pch.h"

#include "shell_debug_log.h"

#include <windows.h>

#include <string>

namespace aegra::shell {

void shell_debug_log(const std::wstring& message) noexcept {
    try {
        ::OutputDebugStringW((L"[aegra_shell] " + message + L"\n").c_str());
        wchar_t temp_dir[MAX_PATH]{};
        const DWORD temp_len = ::GetTempPathW(MAX_PATH, temp_dir);
        if (temp_len == 0 || temp_len >= MAX_PATH) {
            return;
        }
        const std::wstring log_path = std::wstring(temp_dir) + L"aegra_shell_extension.log";
        const HANDLE raw_file = ::CreateFileW(log_path.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ,
                                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (raw_file == INVALID_HANDLE_VALUE) {
            return;
        }
        ATL::CHandle file(raw_file);
        SYSTEMTIME st{};
        ::GetLocalTime(&st);
        wchar_t prefix[64]{};
        swprintf_s(prefix, L"%04u-%02u-%02u %02u:%02u:%02u.%03u ", st.wYear, st.wMonth, st.wDay,
                   st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
        const std::wstring line = std::wstring(prefix) + message + L"\r\n";
        const int utf8_len = ::WideCharToMultiByte(
            CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()), nullptr, 0, nullptr, nullptr);
        if (utf8_len > 0) {
            std::string utf8(static_cast<std::size_t>(utf8_len), '\0');
            ::WideCharToMultiByte(CP_UTF8, 0, line.c_str(), static_cast<int>(line.size()),
                                  utf8.data(), utf8_len, nullptr, nullptr);
            DWORD written = 0;
            ::WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
        }
    } catch (...) {
    }
}

} // namespace aegra::shell
