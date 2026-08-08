#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

namespace aegra::apps::service {

/// Lists authenticated file_set Recovery Point children for Service V4 kind 14.
/// Opens the trusted Archive Group under the repository connection; never returns Archive paths.
[[nodiscard]] base::Result<contracts::RecoveryPointEntryPage>
list_recovery_point_entries(ports::IControlPlaneDatabase& control_plane,
                            ports::IRepositoryStorageFactory& storage_factory,
                            const contracts::ListRecoveryPointEntriesRequest& request,
                            base::CancellationToken cancellation);

} // namespace aegra::apps::service
