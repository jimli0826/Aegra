#pragma once

// WinPE offline-restore control-plane service (kinds 19/20/51/52; Service
// composition root only). Preflight reuses the shared restore-chain helpers;
// arming converts the preflight into a PeRestorePrepareRequest and hands off to
// the application-layer PeRestorePrepareService.

#include "aegra/application/pe_restore_prepare_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/pe_pending_store.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <string>

namespace aegra::apps::service {

struct PeRestoreEnvironment final {
    /// Service data directory (image + pending live beneath `<data_dir>\pe\`).
    std::string data_dir_utf8;
    std::string product_version;
    /// Directory holding aegra_pe_restore.exe / aegra_personal_worker.exe and their
    /// runtime DLL closure (the service executable's own directory).
    std::string payload_directory_utf8;
    std::string boot_entry_name{"Aegra Recovery"};
    /// Diagnostics toggle (AEGRA_PE_DEBUG_SHELL): build the PE image with an
    /// interactive cmd shell instead of auto-launching the executor.
    bool debug_shell{false};
};

class PeRestoreJobService final {
  public:
    PeRestoreJobService(application::ISourceInventoryQuery& source_inventory,
                        ports::IControlPlaneDatabase& control_plane,
                        ports::IRepositoryStorageFactory& storage_factory,
                        application::PeRestorePrepareService& prepare_service,
                        ports::IPePendingJobStore& pending_store, ports::IClock& clock,
                        ports::IRandomSource& random, PeRestoreEnvironment environment,
                        IServiceLog* logger) noexcept;

    /// kind 19: same payload as PrepareRestore, but the target must be the system
    /// disk; issues a regular restore preflight token on success.
    [[nodiscard]] base::Result<contracts::RestorePreflight>
    prepare_pe_restore(const contracts::RestorePreflightRequest& request,
                       base::CancellationToken cancellation);

    /// kind 51: build the image, write the pending job, arm the one-time boot.
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    arm_pe_restore(const contracts::ArmPeRestoreCommand& command,
                   base::CancellationToken cancellation);

    /// kind 20.
    [[nodiscard]] base::Result<contracts::PeRestoreState>
    query_state(base::CancellationToken cancellation);

    /// kind 52: disarm + clear pending. Idempotent.
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    cancel_pe_restore(base::CancellationToken cancellation);

    /// Phase C (service startup): publish the PE-written restore result as an audit
    /// event, then clear the result, the pending files, and any leftover boot entry.
    void publish_boot_result_events(base::CancellationToken cancellation) noexcept;

  private:
    application::ISourceInventoryQuery& source_inventory_;
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
    application::PeRestorePrepareService& prepare_service_;
    ports::IPePendingJobStore& pending_store_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
    PeRestoreEnvironment environment_;
    IServiceLog* logger_;
};

} // namespace aegra::apps::service
