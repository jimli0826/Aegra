#pragma once

#include "aegra/base/error.h"

#include <Windows.h>

#include <string>
#include <utility>

namespace aegra::adapters::windows_disk::detail {

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

[[nodiscard]] inline base::Error win32_error(const DWORD error, std::string operation) {
    base::ErrorCode code = base::ErrorCode::kIoFailure;
    switch (error) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_DRIVE:
        code = base::ErrorCode::kNotFound;
        break;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        // A denied/priv-lacking device or raw-disk operation is an I/O/access
        // failure, never an archive-credential problem (passwords are verified in
        // the archive/crypto layer). Mapping to kUnauthorized here would surface a
        // misleading "re-enter the archive password" prompt for e.g. a
        // write-protected or in-use target disk.
        code = base::ErrorCode::kIoFailure;
        break;
    case ERROR_OPERATION_ABORTED:
        code = base::ErrorCode::kCancelled;
        break;
    default:
        break;
    }
    operation += " failed with Win32 error ";
    operation += std::to_string(error);
    return base::Error{code, std::move(operation)};
}

} // namespace aegra::adapters::windows_disk::detail
