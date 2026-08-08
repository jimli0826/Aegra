#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kServiceRequestSchemaVersion = 4;
inline constexpr std::uint32_t kServiceResponseSchemaVersion = 4;
inline constexpr std::uint32_t kServiceEventSchemaVersion = 4;
inline constexpr std::uint32_t kServiceApiVersion = 4;

enum class ServiceMessageType : std::uint8_t {
    kRequest = 1,
    kResponse = 2,
    kEvent = 3,
};

enum class ServiceRequestKind : std::uint8_t {
    kGetServiceInfo = 1,
    kListRecoveryPoints = 2,
    kListRepositoryConnections = 3,
    kListSourceInventory = 4,
    kListJobs = 5,
    kListSchedules = 6,
    kListEvents = 7,
    kListMountSessions = 8,
    kPrepareRestore = 9,
    kResolveRecoveryPointChain = 10,
    kPlanDeleteRecoveryPoints = 11,
    /// Open archive manifest for a recovery point; returns source volume geometry for Restore UI.
    kGetRecoveryPointLayout = 12,
    kBrowseFileSources = 13,
    kListRecoveryPointEntries = 14,
    kPrepareFileRestore = 15,
    kAddRepositoryConnection = 32,
    kImportRepositoryConnection = 33,
    kTestRepositoryConnection = 34,
    kSetDefaultRepository = 35,
    kRemoveRepositoryConnection = 36,
    kStartBackup = 37,
    kCancelJob = 38,
    kStartVerify = 39,
    kStartRestore = 40,
    kMountRecoveryPoint = 41,
    kUnmountSession = 42,
    kUpsertSchedule = 43,
    kDeleteSchedule = 44,
    kSubscribeTaskEvents = 45,
    kAcknowledgeEvents = 46,
    kExecuteDeletePlan = 47,
    kStartFileRestore = 48,
};

enum class ServiceResponseKind : std::uint8_t {
    kQueryResult = 1,
    kCommandAccepted = 2,
    kRequestFailed = 3,
};

enum class ServiceState : std::uint8_t {
    kStarting = 1,
    kReady = 2,
    kStopping = 3,
};

struct ServiceInfo final {
    std::uint32_t minimum_api_version{kServiceApiVersion};
    std::uint32_t api_version{kServiceApiVersion};
    ServiceState state{ServiceState::kStarting};
    std::string service_version;
    std::vector<std::string> capabilities;
};

using ServiceRequestPayload =
    std::variant<ServiceVersionRange, ServiceRecoveryPointListRequest,
                 RepositoryConnectionListRequest, SourceInventoryListRequest, JobListRequest,
                 ScheduleListRequest, AuditEventListRequest, MountSessionListRequest,
                 RestorePreflightRequest, RecoveryPointRef, RepositoryConnectionInput, ResourceRef,
                 StartBackupCommand, StartVerifyCommand, StartRestoreCommand,
                 MountRecoveryPointCommand, UpsertScheduleCommand, EventSubscriptionRequest,
                 EventAcknowledgement, ExecuteDeletePlanCommand, BrowseFileSourcesRequest,
                 ListRecoveryPointEntriesRequest, PrepareFileRestoreRequest,
                 StartFileRestoreCommand>;

struct ServiceRequest final {
    std::uint32_t schema_version{kServiceRequestSchemaVersion};
    ServiceMessageType message_type{ServiceMessageType::kRequest};
    std::string request_id;
    ServiceRequestKind kind{ServiceRequestKind::kGetServiceInfo};
    std::optional<std::string> idempotency_key;
    ServiceRequestPayload payload{ServiceVersionRange{}};
};

using ServiceResponsePayload =
    std::variant<std::monostate, ServiceInfo, ServiceRecoveryPointPage, RepositoryConnectionPage,
                 SourceInventoryPage, JobPage, SchedulePage, AuditEventPage, MountSessionPage,
                 RestorePreflight, RecoveryPointChainResult, DeletePlanSummary,
                 RecoveryPointLayout, CommandAcknowledgement, FileSourceNodePage,
                 RecoveryPointEntryPage, FileRestorePreflight>;

struct ServiceResponse final {
    std::uint32_t schema_version{kServiceResponseSchemaVersion};
    ServiceMessageType message_type{ServiceMessageType::kResponse};
    std::string request_id;
    ServiceResponseKind kind{ServiceResponseKind::kRequestFailed};
    ServiceRequestKind request_kind{ServiceRequestKind::kGetServiceInfo};
    base::ErrorCode boundary_error_code{base::ErrorCode::kInternal};
    std::string message_code;
    MessageArguments message_arguments;
    ServiceResponsePayload payload{std::monostate{}};
};

enum class ServiceEventKind : std::uint8_t {
    kTaskProgress = 1,
    kTaskCompleted = 2,
    kMountSessionChanged = 3,
};

using ServiceEventPayload = std::variant<TaskProgress, TaskResult, MountSessionSummary>;

struct ServiceEvent final {
    std::uint32_t schema_version{kServiceEventSchemaVersion};
    ServiceMessageType message_type{ServiceMessageType::kEvent};
    std::string subscription_id;
    std::uint64_t sequence{0};
    ServiceEventKind kind{ServiceEventKind::kTaskProgress};
    std::string message_code;
    MessageArguments message_arguments;
    ServiceEventPayload payload{TaskProgress{}};
};

[[nodiscard]] bool is_service_query_kind(ServiceRequestKind kind) noexcept;
[[nodiscard]] bool is_service_command_kind(ServiceRequestKind kind) noexcept;
[[nodiscard]] base::Result<void> validate_service_request(const ServiceRequest& request);
[[nodiscard]] base::Result<void> validate_service_info(const ServiceInfo& service);
[[nodiscard]] base::Result<void> validate_service_response(const ServiceResponse& response);
[[nodiscard]] base::Result<void> validate_service_event(const ServiceEvent& event);

} // namespace aegra::contracts
