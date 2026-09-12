#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <memory>
#include <string_view>

namespace aegra::apps::service {

class BootCheckSupervisor;
class IServiceLog;
class WorkerJobService;

/// Durable post-backup action driver. Claims incomplete post_backup_plans rows
/// (lease-based, restart-safe), submits the planned Verify with a deterministic
/// idempotency key once the backup job reached a successful terminal state, and
/// records every action outcome back into the plan. A completion callback kicks
/// an immediate scan; a background poll covers restarts and capacity retries.
///
/// User-initiated boot checks (StartBootCheck, ADR-0032) reuse the same store:
/// the plan is anchored on a Queued BootCheck JobRecord instead of a backup job,
/// so the run is restart-safe and appears in the task log like any other job.
class PostBackupCoordinator final {
  public:
    PostBackupCoordinator(ports::IControlPlaneDatabase& control_plane,
                          ports::IRepositoryStorageFactory& storage_factory, ports::IClock& clock,
                          ports::IRandomSource& random, IServiceLog* logger);
    ~PostBackupCoordinator();

    PostBackupCoordinator(const PostBackupCoordinator&) = delete;
    PostBackupCoordinator& operator=(const PostBackupCoordinator&) = delete;
    PostBackupCoordinator(PostBackupCoordinator&&) = delete;
    PostBackupCoordinator& operator=(PostBackupCoordinator&&) = delete;

    /// Starts the scan thread. worker_jobs (and boot_check when non-null) must
    /// outlive shutdown(). A null boot_check skips required boot-check actions
    /// with a stable message code.
    void start(WorkerJobService& worker_jobs, BootCheckSupervisor* boot_check = nullptr);

    /// Nudges an immediate scan (called from job completion observers).
    void kick() noexcept;

    /// StartBootCheck (kind 54). Validates the volume_set recovery point against
    /// the Catalog, records a Queued BootCheck job plus its durable plan in one
    /// transaction, and kicks a scan. Replays on a matching idempotency key.
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_boot_check(const contracts::StartBootCheckCommand& command,
                     std::string_view idempotency_key, base::CancellationToken cancellation);

    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
