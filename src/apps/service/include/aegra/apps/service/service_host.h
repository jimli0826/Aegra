#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service.h"
#include "aegra/ports/message_channel.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::application {
class IConnectedRepositoryQuery;
class IPersonalRepositoryQuery;
class IRecoveryPointOperations;
class IRepositoryConnectionService;
class ISourceInventoryQuery;
} // namespace aegra::application

namespace aegra::ports {
class IControlPlaneDatabase;
class IRepositoryStorageFactory;
}

namespace aegra::apps::service {

class IWorkerJobService;
class MountSupervisor;
class ScheduleService;
class WorkerSupervisor;

enum class ServiceLogLevel {
    kTrace,
    kInfo,
    kWarning,
    kError,
};

class IServiceLog {
  public:
    IServiceLog() = default;
    virtual ~IServiceLog() = default;

    IServiceLog(const IServiceLog&) = delete;
    IServiceLog& operator=(const IServiceLog&) = delete;
    IServiceLog(IServiceLog&&) = delete;
    IServiceLog& operator=(IServiceLog&&) = delete;

    virtual void write(ServiceLogLevel level, std::string_view message_code,
                       std::string_view detail) noexcept = 0;
};

struct ServiceRuntimeInfo final {
    std::string service_version;
    std::vector<std::string> capabilities;
    IServiceLog* logger{nullptr};
    application::IPersonalRepositoryQuery* repository_query{nullptr};
    application::IConnectedRepositoryQuery* connected_repository_query{nullptr};
    application::IRepositoryConnectionService* repository_connections{nullptr};
    application::ISourceInventoryQuery* source_inventory{nullptr};
    application::IRecoveryPointOperations* recovery_point_operations{nullptr};
    IWorkerJobService* worker_jobs{nullptr};
    ScheduleService* schedules{nullptr};
    WorkerSupervisor* worker_supervisor{nullptr};
    MountSupervisor* mount_supervisor{nullptr};
    ports::IControlPlaneDatabase* control_plane{nullptr};
    ports::IRepositoryStorageFactory* storage_factory{nullptr};
};

[[nodiscard]] base::Result<contracts::ServiceResponse>
dispatch_service_request(const contracts::ServiceRequest& request,
                         const ServiceRuntimeInfo& runtime,
                         base::CancellationToken cancellation = {});

[[nodiscard]] base::Result<std::string>
handle_service_message(std::string_view encoded_request, const ServiceRuntimeInfo& runtime,
                       base::CancellationToken cancellation = {});

[[nodiscard]] base::Result<void> run_service_session(ports::IMessageChannel& channel,
                                                     const ServiceRuntimeInfo& runtime,
                                                     const base::CancellationToken& cancellation,
                                                     std::size_t maximum_requests = 0);

} // namespace aegra::apps::service
