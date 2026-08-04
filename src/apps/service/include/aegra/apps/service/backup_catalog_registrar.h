#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

namespace aegra::contracts {
struct WorkerResponse;
}

namespace aegra::ports {
class IControlPlaneDatabase;
class IRepositoryStorageFactory;
}

namespace aegra::apps::service {

struct WorkerJobRequest;

class BackupCatalogRegistrar final {
  public:
    BackupCatalogRegistrar(ports::IControlPlaneDatabase& control_plane,
                           ports::IRepositoryStorageFactory& storage_factory) noexcept;

    [[nodiscard]] base::Result<void>
    publish(const WorkerJobRequest& request, const contracts::WorkerResponse& response,
            base::CancellationToken cancellation) const;

  private:
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
};

} // namespace aegra::apps::service
