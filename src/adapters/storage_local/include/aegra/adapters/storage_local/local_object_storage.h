#pragma once

#include "aegra/ports/object_storage.h"
#include "aegra/ports/repository_storage.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>

namespace aegra::adapters::storage_local {
namespace detail {
struct LocalObjectStorageState;
}

enum class LocalRootMode : std::uint8_t {
    kOpenExisting = 1,
    kCreateIfMissing = 2,
};

struct LocalObjectStorageOpenRequest final {
    std::filesystem::path root;
    LocalRootMode mode{LocalRootMode::kOpenExisting};
    std::size_t maximum_read_size{4U * 1024U * 1024U};
};

class LocalObjectStorage final : public ports::IObjectReader,
                                 public ports::IStagedObjectWriter,
                                 public ports::IPrefixEnumerator,
                                 public ports::IObjectPublisher,
                                 public ports::IObjectDeleter,
                                 public ports::IObjectStorageCapabilities {
  public:
    [[nodiscard]] static base::Result<std::unique_ptr<LocalObjectStorage>>
    open(const LocalObjectStorageOpenRequest& request);

    ~LocalObjectStorage() override;
    LocalObjectStorage(const LocalObjectStorage&) = delete;
    LocalObjectStorage& operator=(const LocalObjectStorage&) = delete;
    LocalObjectStorage(LocalObjectStorage&&) = delete;
    LocalObjectStorage& operator=(LocalObjectStorage&&) = delete;

    [[nodiscard]] base::Result<ports::ObjectAttributes>
    get_attributes(std::string_view key, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::size_t>
    read_range(std::string_view key, std::uint64_t offset, std::span<std::byte> destination,
               base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>
    begin_staged_write(std::string_view staging_key, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<ports::ObjectListPage>
    enumerate(const ports::ObjectListRequest& request,
              base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<ports::ObjectAttributes>
    publish(const ports::ObjectPublishRequest& request,
            base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> delete_object(const ports::ObjectDeleteRequest& request,
                                                   base::CancellationToken cancellation) override;
    [[nodiscard]] ports::ObjectStorageCapabilities capabilities() const noexcept override;

  private:
    explicit LocalObjectStorage(std::shared_ptr<detail::LocalObjectStorageState> state) noexcept;

    std::shared_ptr<detail::LocalObjectStorageState> state_;
};

class LocalRepositoryStorageFactory final : public ports::IRepositoryStorageFactory {
  public:
    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    open(std::string_view locator, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    create_empty(std::string_view locator, base::CancellationToken cancellation) override;
};

} // namespace aegra::adapters::storage_local
