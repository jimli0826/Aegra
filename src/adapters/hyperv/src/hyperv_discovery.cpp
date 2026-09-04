#include "aegra/adapters/hyperv/hyperv_discovery.h"

#include <windows.h>

#include <limits>

namespace aegra::adapters::hyperv {
namespace {

class ScHandle final {
  public:
    explicit ScHandle(SC_HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~ScHandle() {
        if (handle_ != nullptr) {
            CloseServiceHandle(handle_);
        }
    }
    ScHandle(const ScHandle&) = delete;
    ScHandle& operator=(const ScHandle&) = delete;
    [[nodiscard]] SC_HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] explicit operator bool() const noexcept { return handle_ != nullptr; }

  private:
    SC_HANDLE handle_{nullptr};
};

[[nodiscard]] std::string wide_to_utf8(const std::wstring& value) {
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

} // namespace

std::string discover_powershell_path() {
    wchar_t system_directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(system_directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    const std::wstring candidate =
        std::wstring(system_directory, length) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    const DWORD attributes = GetFileAttributesW(candidate.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES ||
        (attributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) != 0) {
        return {};
    }
    return wide_to_utf8(candidate);
}

bool is_hyperv_installed() noexcept {
    const ScHandle manager(OpenSCManagerW(nullptr, nullptr, SC_MANAGER_CONNECT));
    if (!manager) {
        return false;
    }
    const ScHandle service(OpenServiceW(manager.get(), L"vmms", SERVICE_QUERY_STATUS));
    return static_cast<bool>(service);
}

} // namespace aegra::adapters::hyperv
