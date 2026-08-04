#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/personal_repository/delete_plan.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

#include <cstdint>
#include <string_view>

namespace aegra::ports {
class IClock;
class IRandomSource;
} // namespace aegra::ports

namespace aegra::application {

class IRecoveryPointOperations {
  public:
    IRecoveryPointOperations() = default;
    virtual ~IRecoveryPointOperations() = default;
    IRecoveryPointOperations(const IRecoveryPointOperations&) = delete;
    IRecoveryPointOperations& operator=(const IRecoveryPointOperations&) = delete;

    [[nodiscard]] virtual base::Result<contracts::RecoveryPointChainResult>
    resolve_chain(const contracts::RecoveryPointRef& reference,
                  base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::DeletePlanSummary>
    plan_delete(const contracts::RecoveryPointRef& reference,
                base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<contracts::CommandAcknowledgement>
    execute_delete(const contracts::ExecuteDeletePlanCommand& command,
                   std::string_view idempotency_key, base::CancellationToken cancellation) = 0;
};

// Chain resolution, delete plan/execute. Does not run Verify Worker (Service job layer does).
class RecoveryPointOperations final : public IRecoveryPointOperations {
  public:
    RecoveryPointOperations(ports::IControlPlaneDatabase& control_plane,
                            ports::IRepositoryStorageFactory& storage_factory, ports::IClock& clock,
                            ports::IRandomSource& random,
                            personal_repository::CatalogScannerLimits limits = {}) noexcept;

    [[nodiscard]] base::Result<contracts::RecoveryPointChainResult>
    resolve_chain(const contracts::RecoveryPointRef& reference,
                  base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<contracts::DeletePlanSummary>
    plan_delete(const contracts::RecoveryPointRef& reference,
                base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    execute_delete(const contracts::ExecuteDeletePlanCommand& command,
                   std::string_view idempotency_key, base::CancellationToken cancellation) override;

  private:
    ports::IControlPlaneDatabase& control_plane_;
    ports::IRepositoryStorageFactory& storage_factory_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
    personal_repository::CatalogScannerLimits limits_;
};

} // namespace aegra::application
