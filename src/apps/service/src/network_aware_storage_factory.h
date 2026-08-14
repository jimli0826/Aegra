#pragma once

#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

#include <memory>

namespace aegra::apps::service {

/// Ensures UNC repository locators are connected (WNet) before local storage open/create.
class NetworkAwareRepositoryStorageFactory final : public ports::IRepositoryStorageFactory {
  public:
    NetworkAwareRepositoryStorageFactory(ports::IRepositoryStorageFactory& inner,
                                         ports::IControlPlaneDatabase& control_plane) noexcept
        : inner_(inner), control_plane_(control_plane) {}

    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    open(std::string_view locator, base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    create_empty(std::string_view locator, base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<std::uint64_t>
    query_free_bytes(std::string_view locator, base::CancellationToken cancellation) override;

  private:
    [[nodiscard]] base::Result<void> ensure_network_access(std::string_view locator,
                                                           base::CancellationToken cancellation);

    ports::IRepositoryStorageFactory& inner_;
    ports::IControlPlaneDatabase& control_plane_;
};

} // namespace aegra::apps::service
