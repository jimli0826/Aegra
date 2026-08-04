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

} // namespace

base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
LocalRepositoryStorageFactory::open(const std::string_view locator,
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
    auto storage = LocalObjectStorage::open({path.value(), LocalRootMode::kOpenExisting});
    if (!storage) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            storage.error());
    }
    return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::success(
        std::make_unique<LocalRepositoryStorageAccess>(std::move(storage).value()));
}

} // namespace aegra::adapters::storage_local
