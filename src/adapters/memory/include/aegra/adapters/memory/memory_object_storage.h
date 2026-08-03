#pragma once

#include "aegra/ports/object_storage.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <string>

namespace aegra::adapters::memory {
namespace detail {
struct MemoryObjectStorageState;
}

struct MemoryObjectStorageOptions final {
    std::size_t maximum_read_size{(std::numeric_limits<std::size_t>::max)()};
    std::optional<std::uint64_t> fail_write_after_bytes;
    bool fail_complete{false};
    std::optional<std::string> fail_publish_destination_key;
    std::optional<std::string> unknown_publish_destination_key;
    std::optional<std::string> fail_delete_key;
    std::optional<std::string> unknown_delete_key;
};

class MemoryObjectStorage final : public ports::IObjectReader,
                                  public ports::IStagedObjectWriter,
                                  public ports::IPrefixEnumerator,
                                  public ports::IObjectPublisher,
                                  public ports::IObjectDeleter,
                                  public ports::IObjectStorageCapabilities {
  public:
    explicit MemoryObjectStorage(MemoryObjectStorageOptions options = {});
    ~MemoryObjectStorage() override;

    MemoryObjectStorage(const MemoryObjectStorage&) = delete;
    MemoryObjectStorage& operator=(const MemoryObjectStorage&) = delete;
    MemoryObjectStorage(MemoryObjectStorage&&) = delete;
    MemoryObjectStorage& operator=(MemoryObjectStorage&&) = delete;

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
    std::shared_ptr<detail::MemoryObjectStorageState> state_;
};

} // namespace aegra::adapters::memory
