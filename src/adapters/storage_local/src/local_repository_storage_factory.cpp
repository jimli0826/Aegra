#include "aegra/adapters/storage_local/local_object_storage.h"

#include <exception>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>

namespace aegra::adapters::storage_local {
namespace {

class LocalRepositoryStorageAccess final : public ports::IRepositoryStorageAccess {
  public:
    explicit LocalRepositoryStorageAccess(std::unique_ptr<LocalObjectStorage> storage) noexcept
        : storage_(std::move(storage)) {}

    [[nodiscard]] ports::IObjectReader& reader() noexcept override { return *storage_; }
    [[nodiscard]] ports::IPrefixEnumerator& enumerator() noexcept override { return *storage_; }
    [[nodiscard]] ports::IStagedObjectWriter& writer() noexcept override { return *storage_; }
    [[nodiscard]] ports::IObjectPublisher& publisher() noexcept override { return *storage_; }
    [[nodiscard]] ports::IObjectDeleter& deleter() noexcept override { return *storage_; }

  private:
    std::unique_ptr<LocalObjectStorage> storage_;
};

[[nodiscard]] base::Result<std::filesystem::path> path_from_utf8(const std::string_view value) {
    if (value.empty()) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "repository locator is empty"});
    }
    try {
        const auto* begin = reinterpret_cast<const char8_t*>(value.data());
        std::filesystem::path path(std::u8string(begin, begin + value.size()));
        if (!path.is_absolute()) {
            return base::Result<std::filesystem::path>::failure(
                {base::ErrorCode::kInvalidArgument, "repository locator must be absolute"});
        }
        return base::Result<std::filesystem::path>::success(std::move(path));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "repository locator is invalid UTF-8"});
    }
}

[[nodiscard]] base::Result<void> require_empty_target(const std::filesystem::path& path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "inspect repository root failed"});
    }
    if (!exists) {
        return base::Result<void>::success();
    }
    if (!std::filesystem::is_directory(path, error) || error) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "repository root is not an empty directory"});
    }
    const bool empty = std::filesystem::is_empty(path, error);
    if (error) {
        return base::Result<void>::failure(
            {base::ErrorCode::kIoFailure, "inspect repository root contents failed"});
    }
    return empty ? base::Result<void>::success()
                 : base::Result<void>::failure(
                       {base::ErrorCode::kConflict, "repository root is not empty"});
}

[[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
open_local_repository(const std::string_view locator, const LocalRootMode mode,
                      const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            {base::ErrorCode::kCancelled, "repository open cancelled"});
    }
    auto path = path_from_utf8(locator);
    if (!path) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            path.error());
    }
    if (mode == LocalRootMode::kCreateIfMissing) {
        auto empty = require_empty_target(path.value());
        if (!empty) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                empty.error());
        }
    }
    auto storage = LocalObjectStorage::open({path.value(), mode});
    if (!storage) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            storage.error());
    }
    return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::success(
        std::make_unique<LocalRepositoryStorageAccess>(std::move(storage).value()));
}

} // namespace

base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
LocalRepositoryStorageFactory::open(const std::string_view locator,
                                    const base::CancellationToken cancellation) {
    return open_local_repository(locator, LocalRootMode::kOpenExisting, cancellation);
}

base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
LocalRepositoryStorageFactory::create_empty(const std::string_view locator,
                                            const base::CancellationToken cancellation) {
    return open_local_repository(locator, LocalRootMode::kCreateIfMissing, cancellation);
}

} // namespace aegra::adapters::storage_local
