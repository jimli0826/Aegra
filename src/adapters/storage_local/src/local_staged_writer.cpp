#include "local_storage_internal.h"

#include <algorithm>
#include <limits>
#include <mutex>
#include <string>
#include <utility>

namespace aegra::adapters::storage_local::detail {
namespace {

class LocalStagedWriteSession final : public ports::IStagedObjectWriteSession {
  public:
    LocalStagedWriteSession(std::filesystem::path partial_path, std::filesystem::path staging_path,
                            UniqueHandle handle)
        : partial_path_(std::move(partial_path)), staging_path_(std::move(staging_path)),
          handle_(std::move(handle)) {}

    ~LocalStagedWriteSession() override { abort(); }

    [[nodiscard]] base::Result<void> write(std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> complete(base::CancellationToken cancellation) override;
    void abort() noexcept override;

  private:
    std::filesystem::path partial_path_;
    std::filesystem::path staging_path_;
    UniqueHandle handle_;
    bool finished_{false};
};

base::Result<void> LocalStagedWriteSession::write(const std::span<const std::byte> source,
                                                  const base::CancellationToken cancellation) {
    if (finished_ || !handle_.valid()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "local staged object is not writable"));
    }
    auto active = check_cancelled(cancellation);
    if (!active) {
        return active;
    }
    std::size_t written = 0;
    while (written < source.size()) {
        active = check_cancelled(cancellation);
        if (!active) {
            return active;
        }
        const auto remaining = source.size() - written;
        const auto requested = static_cast<DWORD>(
            (std::min)(remaining, static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD actual = 0;
        if (!WriteFile(handle_.get(), source.data() + written, requested, &actual, nullptr) ||
            actual == 0) {
            return base::Result<void>::failure(
                win32_error(GetLastError(), "write local staged object"));
        }
        written += actual;
    }
    return base::Result<void>::success();
}

base::Result<void> LocalStagedWriteSession::complete(const base::CancellationToken cancellation) {
    if (finished_ || !handle_.valid()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "local staged object cannot be completed"));
    }
    auto active = check_cancelled(cancellation);
    if (!active) {
        return active;
    }
    if (!FlushFileBuffers(handle_.get())) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "flush local staged object"));
    }
    handle_.reset();
    if (!MoveFileExW(partial_path_.c_str(), staging_path_.c_str(), MOVEFILE_WRITE_THROUGH)) {
        return base::Result<void>::failure(
            win32_error(GetLastError(), "complete local staged object"));
    }
    finished_ = true;
    return base::Result<void>::success();
}

void LocalStagedWriteSession::abort() noexcept {
    if (finished_) {
        return;
    }
    handle_.reset();
    DeleteFileW(partial_path_.c_str());
    finished_ = true;
}

[[nodiscard]] base::Result<void> require_missing(const std::filesystem::path& path,
                                                 const char* message) {
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
        return base::Result<void>::failure(local_error(base::ErrorCode::kConflict, message));
    }
    const auto code = GetLastError();
    if (code != ERROR_FILE_NOT_FOUND && code != ERROR_PATH_NOT_FOUND) {
        return base::Result<void>::failure(win32_error(code, "inspect staged object"));
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>
create_staged_write_session(std::shared_ptr<LocalObjectStorageState> state,
                            const std::string_view staging_key,
                            const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            active.error());
    }
    auto staging_path = object_path(*state, staging_key);
    auto partial_path = internal_write_path(*state, staging_key);
    if (!staging_path || !partial_path) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            !staging_path ? staging_path.error() : partial_path.error());
    }
    const std::unique_lock lock(state->mutex);
    auto staging_parents = ensure_safe_parent_directories(*state, staging_path.value());
    auto partial_parents = ensure_safe_parent_directories(*state, partial_path.value());
    if (!staging_parents || !partial_parents) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            !staging_parents ? staging_parents.error() : partial_parents.error());
    }
    auto staging_missing = require_missing(staging_path.value(), "staging object already exists");
    if (!staging_missing) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            staging_missing.error());
    }
    UniqueHandle handle(CreateFileW(partial_path.value().c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW,
                                    FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
    if (!handle.valid()) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            win32_error(GetLastError(), "create local staged object"));
    }
    return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::success(
        std::make_unique<LocalStagedWriteSession>(
            std::move(partial_path).value(), std::move(staging_path).value(), std::move(handle)));
}

} // namespace aegra::adapters::storage_local::detail
