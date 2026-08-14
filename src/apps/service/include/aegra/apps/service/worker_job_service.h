#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/file_browser.h"

#include <string_view>

namespace aegra::application {
class FileBrowseService;
class ISourceInventoryQuery;
} // namespace aegra::application

namespace aegra::ports {
class IClock;
class IControlPlaneDatabase;
class IRandomSource;
class IRepositoryStorageFactory;
} // namespace aegra::ports

namespace aegra::apps::service {

class WorkerSupervisor;

class IWorkerJobService {
  public:
    IWorkerJobService() = default;
    virtual ~IWorkerJobService() = default;
    IWorkerJobService(const IWorkerJobService&) = delete;
    IWorkerJobService& operator=(const IWorkerJobService&) = delete;

    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    start_backup(const contracts::StartBackupCommand& command, std::string_view idempotency_key,
                 base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    start_verify(const contracts::StartVerifyCommand& command, std::string_view idempotency_key,
                 base::CancellationToken cancellation) = 0;
    /// Restore preflight: `disk.N` (whole disk) or `vol.…` (single volume); tip Full or
    /// Incremental.
    [[nodiscard]] virtual base::Result<contracts::RestorePreflight>
    prepare_restore(const contracts::RestorePreflightRequest& request,
                    base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    start_restore(const contracts::StartRestoreCommand& command, std::string_view idempotency_key,
                  base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::FileRestorePreflight>
    prepare_file_restore(const contracts::PrepareFileRestoreRequest& request,
                         const ports::FileBrowseSession& session,
                         base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    start_file_restore(const contracts::StartFileRestoreCommand& command,
                       std::string_view idempotency_key, base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    cancel_job(const contracts::ResourceRef& job, std::string_view idempotency_key,
               base::CancellationToken cancellation) = 0;
};

class WorkerJobService final : public IWorkerJobService {
  public:
    WorkerJobService(application::ISourceInventoryQuery& source_inventory,
                     ports::IControlPlaneDatabase& control_plane,
                     ports::IRepositoryStorageFactory& storage_factory,
                     WorkerSupervisor& supervisor, ports::IClock& clock,
                     ports::IRandomSource& random,
                     application::FileBrowseService* file_browse = nullptr) noexcept;

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_backup(const contracts::StartBackupCommand& command, std::string_view idempotency_key,
                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_verify(const contracts::StartVerifyCommand& command, std::string_view idempotency_key,
                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::RestorePreflight>
    prepare_restore(const contracts::RestorePreflightRequest& request,
                    base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_restore(const contracts::StartRestoreCommand& command, std::string_view idempotency_key,
                  base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::FileRestorePreflight>
    prepare_file_restore(const contracts::PrepareFileRestoreRequest& request,
                         const ports::FileBrowseSession& session,
                         base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_file_restore(const contracts::StartFileRestoreCommand& command,
                       std::string_view idempotency_key,
                       base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    cancel_job(const contracts::ResourceRef& job, std::string_view idempotency_key,
               base::CancellationToken cancellation) override;

  private:
    application::ISourceInventoryQuery& source_inventory_;
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
    WorkerSupervisor& supervisor_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
    application::FileBrowseService* file_browse_;
};

} // namespace aegra::apps::service
