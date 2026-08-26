#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/pe_restore.h"
#include "aegra/ports/pe_pending_store.h"

#include <Windows.h>

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::windows_pe::detail {

inline constexpr std::wstring_view kPendingJobFileName = L"restore_job.v1.json";
inline constexpr std::wstring_view kPendingKeyFileName = L"restore_job.v1.key";
inline constexpr std::wstring_view kPendingResultFileName = L"restore_result.v1.json";
/// Volume-relative pending directory probed by the WinPE-side volume scan.
inline constexpr std::wstring_view kDefaultDataDirRelative = L"ProgramData\\Aegra";
inline constexpr std::wstring_view kPendingDirRelative = L"pe\\pending";

/// Upper bounds for documents read back from disk (fail closed on oversized files).
inline constexpr std::size_t kMaximumJobDocumentSize = 1024 * 1024;
inline constexpr std::size_t kMaximumResultDocumentSize = 256 * 1024;

class UniqueHandle final {
  public:
    UniqueHandle() noexcept = default;
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
    [[nodiscard]] HANDLE release() noexcept {
        const auto value = handle_;
        handle_ = INVALID_HANDLE_VALUE;
        return value;
    }
    void reset(HANDLE replacement = INVALID_HANDLE_VALUE) noexcept {
        if (valid()) {
            CloseHandle(handle_);
        }
        handle_ = replacement;
    }

  private:
    HANDLE handle_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] base::Error pe_store_error(base::ErrorCode code, const char* message);
[[nodiscard]] base::Error win32_error(DWORD code, const char* operation);

[[nodiscard]] base::Result<std::wstring> utf8_to_wide(std::string_view value);
[[nodiscard]] base::Result<std::string> wide_to_utf8(std::wstring_view value);

/// Whole-file read with a hard size bound; kNotFound when the file does not exist.
[[nodiscard]] base::Result<std::vector<std::byte>> read_bounded_file(const std::wstring& path,
                                                                     std::size_t maximum_size);

/// Atomic document write: temp sibling + FlushFileBuffers + MoveFileExW.
[[nodiscard]] base::Result<void> write_document_atomically(const std::wstring& path,
                                                           std::string_view content,
                                                           bool replace_existing);

/// Create-only write of the sealed job key with an explicit SYSTEM/Administrators DACL.
[[nodiscard]] base::Result<void> write_key_file(const std::wstring& path,
                                                std::span<const std::byte> job_key);

/// Overwrite the key file content in place, then delete it. Missing file succeeds.
[[nodiscard]] base::Result<void> scrub_and_delete_key_file(const std::wstring& path);

/// Delete a file; ERROR_FILE_NOT_FOUND / ERROR_PATH_NOT_FOUND succeed.
[[nodiscard]] base::Result<void> delete_file_if_exists(const std::wstring& path);

[[nodiscard]] bool file_exists(const std::wstring& path) noexcept;

/// Creates every missing component of `directory` (absolute path).
[[nodiscard]] base::Result<void> ensure_directory_exists(const std::wstring& directory);

/// Store construction shared with the volume-scan factory (pe_pending_store.cpp).
[[nodiscard]] std::unique_ptr<ports::IPePendingJobStore>
make_pe_pending_store_for_directory(std::wstring pending_directory);

// JSON codec (pe_pending_json.cpp). Encoding validates first; decoding validates last.
[[nodiscard]] base::Result<std::string> encode_pe_pending_job(const contracts::PePendingJobV1& job);
[[nodiscard]] base::Result<contracts::PePendingJobV1>
decode_pe_pending_job(std::string_view json_text);
[[nodiscard]] base::Result<std::string>
encode_pe_restore_result(const contracts::PeRestoreResultV1& result);
[[nodiscard]] base::Result<contracts::PeRestoreResultV1>
decode_pe_restore_result(std::string_view json_text);

} // namespace aegra::adapters::windows_pe::detail
