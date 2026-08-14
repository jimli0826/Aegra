#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

namespace aegra::ports {
class IRandomSource;
}

namespace aegra::application {

class IRepositoryConnectionService {
  public:
    IRepositoryConnectionService() = default;
    virtual ~IRepositoryConnectionService() = default;
    IRepositoryConnectionService(const IRepositoryConnectionService&) = delete;
    IRepositoryConnectionService& operator=(const IRepositoryConnectionService&) = delete;
    IRepositoryConnectionService(IRepositoryConnectionService&&) = delete;
    IRepositoryConnectionService& operator=(IRepositoryConnectionService&&) = delete;

    [[nodiscard]] virtual base::Result<contracts::RepositoryConnectionPage>
    list_connections(const contracts::RepositoryConnectionListRequest& request,
                     base::CancellationToken cancellation) = 0;

    // Creates a control-plane connection reference. Does not create Repository objects.
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    add_connection(const contracts::RepositoryConnectionInput& input,
                   std::string_view idempotency_key, base::CancellationToken cancellation) = 0;

    // Imports an existing Repository root; requires a readable Repository Descriptor.
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    import_connection(const contracts::RepositoryConnectionInput& input,
                      std::string_view idempotency_key, base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    test_connection(const contracts::ResourceRef& reference, std::string_view idempotency_key,
                    base::CancellationToken cancellation) = 0;

    // Persists the timeout result without touching Repository storage. Used by the Service
    // request executor when a connection probe exceeds its deadline.
    [[nodiscard]] virtual base::Result<void>
    mark_connection_unavailable(const contracts::ResourceRef& reference,
                                base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    set_default_connection(const contracts::ResourceRef& reference,
                           std::string_view idempotency_key,
                           base::CancellationToken cancellation) = 0;

    // Removes control-plane reference only; never deletes .bkf or catalog objects.
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    remove_connection(const contracts::ResourceRef& reference, std::string_view idempotency_key,
                      base::CancellationToken cancellation) = 0;
};

class RepositoryConnectionService final : public IRepositoryConnectionService {
  public:
    RepositoryConnectionService(ports::IControlPlaneDatabase& control_plane,
                                ports::IRepositoryStorageFactory& storage_factory,
                                ports::IClock& clock, ports::IRandomSource& random) noexcept;
    ~RepositoryConnectionService() override = default;

    RepositoryConnectionService(const RepositoryConnectionService&) = delete;
    RepositoryConnectionService& operator=(const RepositoryConnectionService&) = delete;
    RepositoryConnectionService(RepositoryConnectionService&&) = delete;
    RepositoryConnectionService& operator=(RepositoryConnectionService&&) = delete;

    [[nodiscard]] base::Result<contracts::RepositoryConnectionPage>
    list_connections(const contracts::RepositoryConnectionListRequest& request,
                     base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    add_connection(const contracts::RepositoryConnectionInput& input,
                   std::string_view idempotency_key, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    import_connection(const contracts::RepositoryConnectionInput& input,
                      std::string_view idempotency_key,
                      base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    test_connection(const contracts::ResourceRef& reference, std::string_view idempotency_key,
                    base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void>
    mark_connection_unavailable(const contracts::ResourceRef& reference,
                                base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    set_default_connection(const contracts::ResourceRef& reference,
                           std::string_view idempotency_key,
                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    remove_connection(const contracts::ResourceRef& reference, std::string_view idempotency_key,
                      base::CancellationToken cancellation) override;

  private:
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
};

} // namespace aegra::application
