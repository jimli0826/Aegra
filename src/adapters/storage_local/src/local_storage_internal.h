#pragma once

#include "aegra/adapters/storage_local/local_object_storage.h"

#include <Windows.h>

#include <filesystem>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

namespace aegra::adapters::storage_local::detail {

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

struct DeleteOperation final {
    std::optional<std::string> expected_generation;
};

struct LocalObjectStorageState final {
    std::filesystem::path root;
    std::size_t maximum_read_size{0};
    mutable std::shared_mutex mutex;
    std::map<std::pair<std::string, std::string>, DeleteOperation> delete_operations;
};

[[nodiscard]] base::Error local_error(base::ErrorCode code, const char* message);
[[nodiscard]] base::Error win32_error(DWORD code, const char* operation);
[[nodiscard]] base::Result<void> check_cancelled(base::CancellationToken cancellation);

[[nodiscard]] bool is_local_object_key(std::string_view key) noexcept;
[[nodiscard]] bool is_local_object_prefix(std::string_view prefix) noexcept;
[[nodiscard]] bool is_internal_component(std::wstring_view component) noexcept;
[[nodiscard]] base::Result<std::filesystem::path> object_path(const LocalObjectStorageState& state,
                                                              std::string_view key);
[[nodiscard]] base::Result<std::filesystem::path>
internal_write_path(const LocalObjectStorageState& state, std::string_view key);
[[nodiscard]] base::Result<std::string> object_key(const LocalObjectStorageState& state,
                                                   const std::filesystem::path& path);

[[nodiscard]] base::Result<void>
ensure_safe_parent_directories(const LocalObjectStorageState& state,
                               const std::filesystem::path& path);
[[nodiscard]] base::Result<void>
validate_safe_parent_directories(const LocalObjectStorageState& state,
                                 const std::filesystem::path& path);
[[nodiscard]] base::Result<UniqueHandle> open_regular_file(const LocalObjectStorageState& state,
                                                           const std::filesystem::path& path,
                                                           DWORD desired_access);
[[nodiscard]] base::Result<ports::ObjectAttributes> attributes_from_handle(std::string_view key,
                                                                           HANDLE handle);
[[nodiscard]] base::Result<ports::ObjectAttributes>
attributes_for_path(const LocalObjectStorageState& state, std::string_view key,
                    const std::filesystem::path& path);
[[nodiscard]] base::Result<std::shared_ptr<LocalObjectStorageState>>
open_local_state(const LocalObjectStorageOpenRequest& request);

[[nodiscard]] base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>
create_staged_write_session(std::shared_ptr<LocalObjectStorageState> state,
                            std::string_view staging_key, base::CancellationToken cancellation);

} // namespace aegra::adapters::storage_local::detail
