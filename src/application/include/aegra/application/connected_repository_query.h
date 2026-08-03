#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

namespace aegra::application {

class IConnectedRepositoryQuery {
  public:
    IConnectedRepositoryQuery() = default;
    virtual ~IConnectedRepositoryQuery() = default;
    IConnectedRepositoryQuery(const IConnectedRepositoryQuery&) = delete;
    IConnectedRepositoryQuery& operator=(const IConnectedRepositoryQuery&) = delete;
    IConnectedRepositoryQuery(IConnectedRepositoryQuery&&) = delete;
    IConnectedRepositoryQuery& operator=(IConnectedRepositoryQuery&&) = delete;

    // When repository_connection_id is null, uses the default connection if present.
    [[nodiscard]] virtual base::Result<contracts::ServiceRecoveryPointPage>
    list_recovery_points(const contracts::ServiceRecoveryPointListRequest& request,
                         base::CancellationToken cancellation) = 0;
};

// Multi-connection Recovery Point query. Catalog remains the RP authority; control plane only
// supplies connection locators and SecretRef handles.
class ConnectedRepositoryQuery final : public IConnectedRepositoryQuery {
  public:
    ConnectedRepositoryQuery(ports::IControlPlaneDatabase& control_plane,
                             ports::IRepositoryStorageFactory& storage_factory,
                             personal_repository::CatalogScannerLimits limits = {}) noexcept;
    ~ConnectedRepositoryQuery() override = default;

    ConnectedRepositoryQuery(const ConnectedRepositoryQuery&) = delete;
    ConnectedRepositoryQuery& operator=(const ConnectedRepositoryQuery&) = delete;
    ConnectedRepositoryQuery(ConnectedRepositoryQuery&&) = delete;
    ConnectedRepositoryQuery& operator=(ConnectedRepositoryQuery&&) = delete;

    [[nodiscard]] base::Result<contracts::ServiceRecoveryPointPage>
    list_recovery_points(const contracts::ServiceRecoveryPointListRequest& request,
                         base::CancellationToken cancellation) override;

  private:
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
    personal_repository::CatalogScannerLimits limits_;
};

} // namespace aegra::application
