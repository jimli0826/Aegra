#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"

#include <memory>

namespace aegra::apps::service {

class BootCheckSupervisor;
class IServiceLog;
class WorkerJobService;

/// Durable post-backup action driver. Claims incomplete post_backup_plans rows
/// (lease-based, restart-safe), submits the planned Verify with a deterministic
/// idempotency key once the backup job reached a successful terminal state, and
/// records every action outcome back into the plan. A completion callback kicks
/// an immediate scan; a background poll covers restarts and capacity retries.
class PostBackupCoordinator final {
  public:
    PostBackupCoordinator(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
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

    void shutdown() noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::apps::service
