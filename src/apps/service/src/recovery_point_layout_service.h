#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

namespace aegra::apps::service {

/// Loads Manifest volumes for a recovery point (opens the personal archive with Service-managed
/// credentials). Used by Restore UI Source Disks.
[[nodiscard]] base::Result<contracts::RecoveryPointLayout>
load_recovery_point_layout(ports::IControlPlaneDatabase& control_plane,
                           ports::IRepositoryStorageFactory& storage_factory,
                           const contracts::RecoveryPointRef& reference,
                           base::CancellationToken cancellation);

} // namespace aegra::apps::service
