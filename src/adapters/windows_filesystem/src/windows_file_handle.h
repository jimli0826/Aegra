#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <Windows.h>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {

class UniqueHandle final {
  public:
    UniqueHandle() = default;
    explicit UniqueHandle(HANDLE handle) noexcept : handle_(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            reset(other.release());
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, INVALID_HANDLE_VALUE); }
    void reset(HANDLE handle = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = handle;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] base::Error win32_error(DWORD error, std::string operation);
[[nodiscard]] base::Result<UniqueHandle>
open_path(const std::vector<std::uint16_t>& utf16_path, DWORD access, DWORD share, DWORD disposition,
          DWORD flags);
/// Ensures a directory root path ends with '\\' so CreateFile opens the root directory
/// (required for VSS device objects and Volume GUID roots). Strips embedded NULs.
[[nodiscard]] std::vector<std::uint16_t>
ensure_trailing_directory_separator(std::vector<std::uint16_t> utf16_path);
[[nodiscard]] base::Result<void> ensure_ntfs_or_refs(const UniqueHandle& root);
/// Path-based FS check (trailing '\\' added if missing). Used when ByHandle fails on
/// some VSS/device roots with ERROR_INVALID_FUNCTION.
[[nodiscard]] base::Result<void> ensure_ntfs_or_refs_path(const std::wstring& directory_root);

} // namespace aegra::adapters::windows_filesystem::detail
