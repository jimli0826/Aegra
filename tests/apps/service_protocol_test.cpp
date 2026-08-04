#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/apps/service/worker_job_service.h"

#include "aegra/application/recovery_point_operations.h"
#include "aegra/base/error.h"
#include "aegra/contracts/service.h"
#include "aegra/ports/message_channel.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace app = aegra::apps::service;
namespace base = aegra::base;
namespace contracts = aegra::contracts;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

app::ServiceRuntimeInfo runtime_info() {
    return {.service_version = "0.1.0", .capabilities = {"repository.list", "service.info"}};
}

class ObservedRecoveryPointOperations final : public aegra::application::IRecoveryPointOperations {
  public:
    [[nodiscard]] base::Result<contracts::RecoveryPointChainResult>
    resolve_chain(const contracts::RecoveryPointRef&, base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::RecoveryPointChainResult>::success({});
    }

    [[nodiscard]] base::Result<contracts::DeletePlanSummary>
    plan_delete(const contracts::RecoveryPointRef&, base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::DeletePlanSummary>::success({});
    }

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    execute_delete(const contracts::ExecuteDeletePlanCommand&, std::string_view,
                   base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::CommandAcknowledgement>::success({});
    }

    std::size_t calls{0};
};

class ObservedWorkerJobService final : public app::IWorkerJobService {
  public:
    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_backup(const contracts::StartBackupCommand&, std::string_view,
                 base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::CommandAcknowledgement>::success({});
    }

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    start_verify(const contracts::StartVerifyCommand&, std::string_view,
                 base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::CommandAcknowledgement>::success({});
    }

    [[nodiscard]] base::Result<contracts::CommandAcknowledgement>
    cancel_job(const contracts::ResourceRef&, std::string_view, base::CancellationToken) override {
        ++calls;
        return base::Result<contracts::CommandAcknowledgement>::success({});
    }

    std::size_t calls{0};
};

contracts::ServiceRequest service_request() {
    contracts::ServiceRequest request;
    request.request_id = "request-1";
    return request;
}

bool test_request_codec() {
    auto encoded = app::encode_service_request(service_request());
    const std::string golden =
        R"({"idempotency_key":null,"kind":1,"message_type":1,"payload":{"maximum_api_version":3,"minimum_api_version":3},"request_id":"request-1","schema_version":3})";
    bool passed = expect(encoded && encoded.value() == golden,
                         "service request encoding has a stable V3 envelope");
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    passed &=
        expect(decoded && decoded.value().request_id == "request-1" &&
                   std::holds_alternative<contracts::ServiceVersionRange>(decoded.value().payload),
               "service request roundtrips");
    passed &= expect(
        !app::decode_service_request(
            R"({"schema_version":3,"message_type":1,"request_id":"request-1","kind":1,"idempotency_key":null,"payload":{"minimum_api_version":3,"maximum_api_version":3},"extra":0})"),
        "unknown request fields are rejected");
    passed &= expect(!app::decode_service_request("{"), "malformed request JSON is rejected");
    const std::string oversized(app::kMaximumServiceFrameBytes + 1, 'x');
    passed &= expect(!app::decode_service_request(oversized),
                     "oversized request is rejected before parsing");
    return passed;
}

bool test_response_codec_and_dispatch() {
    auto dispatched = app::dispatch_service_request(service_request(), runtime_info());
    const auto* service =
        dispatched ? std::get_if<contracts::ServiceInfo>(&dispatched.value().payload) : nullptr;
    bool passed = expect(service != nullptr && service->service_version == "0.1.0",
                         "GetServiceInfo dispatches to owned runtime data");
    auto encoded = dispatched ? app::encode_service_response(dispatched.value())
                              : app::encode_service_response({});
    auto decoded =
        encoded ? app::decode_service_response(encoded.value()) : app::decode_service_response({});
    const auto* decoded_service =
        decoded ? std::get_if<contracts::ServiceInfo>(&decoded.value().payload) : nullptr;
    passed &= expect(decoded_service != nullptr && decoded.value().request_id == "request-1" &&
                         decoded_service->capabilities ==
                             std::vector<std::string>{"repository.list", "service.info"},
                     "service response roundtrips with stable capability data");

    auto incompatible = service_request();
    incompatible.payload = contracts::ServiceVersionRange{4, 5};
    auto rejected = app::dispatch_service_request(incompatible, runtime_info());
    passed &= expect(
        rejected && rejected.value().kind == contracts::ServiceResponseKind::kRequestFailed &&
            rejected.value().boundary_error_code == base::ErrorCode::kUnsupportedVersion &&
            rejected.value().message_code == "service.api_version_unsupported",
        "API negotiation returns a structured unsupported response");
    return passed;
}

bool test_repository_list_codec_and_dispatch() {
    contracts::ServiceRequest request;
    request.request_id = "repository-request-1";
    request.kind = contracts::ServiceRequestKind::kListRecoveryPoints;
    request.payload = contracts::ServiceRecoveryPointListRequest{std::nullopt, {25, std::nullopt}};
    auto encoded = app::encode_service_request(request);
    bool passed =
        expect(encoded && encoded.value().find("\"maximum_results\":25") != std::string::npos,
               "repository query encodes its bounded page payload");
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    const auto* list =
        decoded ? std::get_if<contracts::ServiceRecoveryPointListRequest>(&decoded.value().payload)
                : nullptr;
    passed &= expect(list != nullptr && list->page.maximum_results == 25,
                     "repository list request roundtrips");
    auto response = decoded ? app::dispatch_service_request(decoded.value(), runtime_info())
                            : app::dispatch_service_request({}, runtime_info());
    const auto* page =
        response ? std::get_if<contracts::ServiceRecoveryPointPage>(&response.value().payload)
                 : nullptr;
    passed &= expect(page != nullptr &&
                         page->catalog.state == contracts::RepositoryCatalogState::kNotConfigured,
                     "unconfigured Service returns a valid empty repository page");
    auto response_json = response ? app::encode_service_response(response.value())
                                  : app::encode_service_response({});
    auto roundtrip = response_json ? app::decode_service_response(response_json.value())
                                   : app::decode_service_response({});
    passed &= expect(roundtrip &&
                         std::holds_alternative<contracts::ServiceRecoveryPointPage>(
                             roundtrip.value().payload) &&
                         roundtrip.value().request_id == "repository-request-1",
                     "repository page response roundtrips with request correlation");
    return passed;
}

bool test_command_codec_and_unavailable_dispatch() {
    contracts::ServiceRequest request;
    request.request_id = "command-request-1";
    request.kind = contracts::ServiceRequestKind::kStartBackup;
    request.idempotency_key = "backup-command-1";
    request.payload =
        contracts::StartBackupCommand{.source_id = "source:volume-1",
                                      .repository_connection_id = "repository:primary",
                                      .backup_type = contracts::BackupType::kFull};
    auto encoded = app::encode_service_request(request);
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    auto response = decoded ? app::dispatch_service_request(decoded.value(), runtime_info())
                            : app::dispatch_service_request({}, runtime_info());
    return expect(decoded && std::holds_alternative<contracts::StartBackupCommand>(
                                 decoded.value().payload),
                  "known command roundtrips") &&
           expect(response &&
                      response.value().kind == contracts::ServiceResponseKind::kRequestFailed &&
                      response.value().boundary_error_code == base::ErrorCode::kConflict &&
                      response.value().message_code == "service.capability_unavailable",
                  "known but unavailable command is rejected without side effects");
}

bool test_disabled_s5_capabilities_block_injected_handlers() {
    ObservedRecoveryPointOperations recovery_operations;
    ObservedWorkerJobService worker_jobs;
    auto runtime = runtime_info();
    runtime.recovery_point_operations = &recovery_operations;
    runtime.worker_jobs = &worker_jobs;

    const std::array requests{
        contracts::ServiceRequest{.request_id = "chain-request",
                                  .kind = contracts::ServiceRequestKind::kResolveRecoveryPointChain,
                                  .payload =
                                      contracts::RecoveryPointRef{"repository:1", "recovery:1"}},
        contracts::ServiceRequest{.request_id = "plan-request",
                                  .kind = contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints,
                                  .payload =
                                      contracts::RecoveryPointRef{"repository:1", "recovery:1"}},
        contracts::ServiceRequest{.request_id = "verify-request",
                                  .kind = contracts::ServiceRequestKind::kStartVerify,
                                  .idempotency_key = "verify-command",
                                  .payload =
                                      contracts::StartVerifyCommand{"repository:1", "recovery:1"}},
        contracts::ServiceRequest{.request_id = "delete-request",
                                  .kind = contracts::ServiceRequestKind::kExecuteDeletePlan,
                                  .idempotency_key = "delete-command",
                                  .payload =
                                      contracts::ExecuteDeletePlanCommand{"plan-token", true}},
    };

    bool passed = true;
    for (const auto& request : requests) {
        const auto response = app::dispatch_service_request(request, runtime);
        passed &= expect(
            response && response.value().kind == contracts::ServiceResponseKind::kRequestFailed &&
                response.value().boundary_error_code == base::ErrorCode::kConflict &&
                response.value().message_code == "service.capability_unavailable",
            "disabled S5 capability blocks its injected handler");
    }
    return passed && expect(recovery_operations.calls == 0 && worker_jobs.calls == 0,
                            "disabled S5 requests produce no handler side effects");
}

bool request_roundtrips(const contracts::ServiceRequestKind kind,
                        contracts::ServiceRequestPayload payload, const bool command) {
    contracts::ServiceRequest request;
    request.request_id = "matrix-request-1";
    request.kind = kind;
    request.payload = std::move(payload);
    if (command) {
        request.idempotency_key = "matrix-command-1";
    }
    auto encoded = app::encode_service_request(request);
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    return decoded && decoded.value().kind == kind &&
           decoded.value().payload.index() == request.payload.index();
}

bool test_planned_request_payload_codecs() {
    bool passed = true;
    const contracts::ServicePageRequest page{25, std::nullopt};
    passed &=
        request_roundtrips(contracts::ServiceRequestKind::kListRepositoryConnections,
                           contracts::RepositoryConnectionListRequest{page, std::nullopt}, false);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kListSourceInventory,
                                 contracts::SourceInventoryListRequest{page, true}, false);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kListJobs,
                                 contracts::JobListRequest{page, contracts::JobOperation::kBackup,
                                                           contracts::ServiceJobState::kRunning},
                                 false);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kListSchedules,
                                 contracts::ScheduleListRequest{page, true}, false);
    passed &= request_roundtrips(
        contracts::ServiceRequestKind::kListEvents,
        contracts::AuditEventListRequest{page, contracts::AuditSeverity::kWarning, 1, 2, "trace:1"},
        false);
    passed &= request_roundtrips(
        contracts::ServiceRequestKind::kListMountSessions,
        contracts::MountSessionListRequest{page, contracts::MountSessionState::kMounted}, false);
    passed &= request_roundtrips(
        contracts::ServiceRequestKind::kPrepareRestore,
        contracts::RestorePreflightRequest{"recovery:1", "source:target-1"}, false);
    const contracts::RecoveryPointRef recovery_ref{"repository:1", "recovery:1"};
    passed &= request_roundtrips(contracts::ServiceRequestKind::kResolveRecoveryPointChain,
                                 recovery_ref, false);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints,
                                 recovery_ref, false);

    const contracts::RepositoryConnectionInput repository_input{.display_name = "Local Repository",
                                                                .locator = "repository:local",
                                                                .credential_ref = std::nullopt};
    passed &= request_roundtrips(contracts::ServiceRequestKind::kAddRepositoryConnection,
                                 repository_input, true);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kImportRepositoryConnection,
                                 repository_input, true);

    constexpr std::array reference_commands{
        contracts::ServiceRequestKind::kTestRepositoryConnection,
        contracts::ServiceRequestKind::kSetDefaultRepository,
        contracts::ServiceRequestKind::kRemoveRepositoryConnection,
        contracts::ServiceRequestKind::kCancelJob,
        contracts::ServiceRequestKind::kUnmountSession,
        contracts::ServiceRequestKind::kDeleteSchedule,
    };
    for (const auto kind : reference_commands) {
        passed &= request_roundtrips(kind, contracts::ResourceRef{"resource:1"}, true);
    }
    passed &= request_roundtrips(contracts::ServiceRequestKind::kStartVerify,
                                 contracts::StartVerifyCommand{"repository:1", "recovery:1"}, true);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kExecuteDeletePlan,
                                 contracts::ExecuteDeletePlanCommand{"plan-token-1", true}, true);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kStartRestore,
                                 contracts::StartRestoreCommand{"preflight-token"}, true);
    passed &= request_roundtrips(
        contracts::ServiceRequestKind::kMountRecoveryPoint,
        contracts::MountRecoveryPointCommand{"recovery:1", std::string("M")}, true);
    passed &= request_roundtrips(
        contracts::ServiceRequestKind::kUpsertSchedule,
        contracts::UpsertScheduleCommand{
            .display_name = "Daily backup",
            .enabled = true,
            .source_id = "source:1",
            .repository_connection_id = "repository:1",
            .backup_type = contracts::BackupType::kFull,
            .trigger = {contracts::ScheduleTriggerKind::kDaily, 120, 0, "Asia/Shanghai"}},
        true);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kSubscribeTaskEvents,
                                 contracts::EventSubscriptionRequest{std::nullopt, 0, 64}, true);
    passed &= request_roundtrips(contracts::ServiceRequestKind::kAcknowledgeEvents,
                                 contracts::EventAcknowledgement{"subscription:1", 7}, true);
    return expect(passed, "all planned request payload variants roundtrip");
}

bool query_response_roundtrips(const contracts::ServiceRequestKind kind,
                               contracts::ServiceResponsePayload payload) {
    contracts::ServiceResponse response;
    response.request_id = "matrix-response-1";
    response.kind = contracts::ServiceResponseKind::kQueryResult;
    response.request_kind = kind;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "service.query_ready";
    response.payload = std::move(payload);
    auto encoded = app::encode_service_response(response);
    auto decoded =
        encoded ? app::decode_service_response(encoded.value()) : app::decode_service_response({});
    return decoded && decoded.value().request_kind == kind &&
           decoded.value().payload.index() == response.payload.index();
}

bool test_planned_response_payload_codecs() {
    bool passed = query_response_roundtrips(
        contracts::ServiceRequestKind::kListRecoveryPoints,
        contracts::ServiceRecoveryPointPage{std::nullopt, contracts::RecoveryPointPage{}});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kListRepositoryConnections,
                                        contracts::RepositoryConnectionPage{});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kListSourceInventory,
                                        contracts::SourceInventoryPage{});
    passed &=
        query_response_roundtrips(contracts::ServiceRequestKind::kListJobs, contracts::JobPage{});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kListSchedules,
                                        contracts::SchedulePage{});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kListEvents,
                                        contracts::AuditEventPage{});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kListMountSessions,
                                        contracts::MountSessionPage{});
    passed &= query_response_roundtrips(
        contracts::ServiceRequestKind::kResolveRecoveryPointChain,
        contracts::RecoveryPointChainResult{
            .repository_connection_id = "repository:1",
            .recovery_point_id = "recovery:1",
            .layers = {{"recovery:1", contracts::BackupType::kFull, std::nullopt,
                        contracts::RecoveryPointStructuralState::kComplete,
                        contracts::RecoveryPointAuthenticationState::kNotAttempted,
                        contracts::RecoveryPointChainCompleteness::kComplete}},
            .restore_eligible = false,
            .mount_eligible = false,
            .verify_eligible = true,
            .message_code = "recovery_point.chain_ready"});
    passed &= query_response_roundtrips(
        contracts::ServiceRequestKind::kPlanDeleteRecoveryPoints,
        contracts::DeletePlanSummary{.plan_token = "plan-token-1",
                                     .operation_id = "del-1",
                                     .repository_connection_id = "repository:1",
                                     .root_recovery_point_id = "recovery:1",
                                     .targets = {{"recovery:1", 1, 1}},
                                     .expires_utc_ms = 1});
    passed &= query_response_roundtrips(contracts::ServiceRequestKind::kPrepareRestore,
                                        contracts::RestorePreflight{"preflight-token", "recovery:1",
                                                                    "source:target-1", 100, 1,
                                                                    1'800'000'000'000ULL});
    return expect(passed, "all planned query response payload variants roundtrip");
}

bool test_command_acknowledgement_codec() {
    contracts::ServiceResponse response;
    response.request_id = "subscription-request-1";
    response.kind = contracts::ServiceResponseKind::kCommandAccepted;
    response.request_kind = contracts::ServiceRequestKind::kSubscribeTaskEvents;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "task.events_subscribed";
    response.payload =
        contracts::CommandAcknowledgement{.command_id = "command:subscription-1",
                                          .disposition = contracts::CommandDisposition::kAccepted,
                                          .resource_id = "subscription:1",
                                          .event_subscription = contracts::EventSubscriptionLease{
                                              "subscription:1", "resume-token", 1, 64}};
    auto encoded = app::encode_service_response(response);
    auto decoded =
        encoded ? app::decode_service_response(encoded.value()) : app::decode_service_response({});
    const auto* acknowledgement =
        decoded ? std::get_if<contracts::CommandAcknowledgement>(&decoded.value().payload)
                : nullptr;
    return expect(acknowledgement != nullptr && acknowledgement->event_subscription &&
                      acknowledgement->event_subscription->maximum_unacknowledged_events == 64,
                  "command acknowledgement and subscription lease roundtrip");
}

bool test_event_codec() {
    contracts::TaskProgress progress;
    progress.job_id = "job:backup-1";
    progress.trace_id = "trace:backup-1";
    progress.phase = contracts::TaskPhase::kWriting;
    progress.logical_bytes = 100;
    progress.processed_bytes = 50;
    progress.message_code = "backup.writing";
    contracts::ServiceEvent event;
    event.subscription_id = "subscription:desktop-1";
    event.sequence = 7;
    event.message_code = "backup.writing";
    event.payload = progress;
    auto encoded = app::encode_service_event(event);
    auto decoded =
        encoded ? app::decode_service_event(encoded.value()) : app::decode_service_event({});
    const auto* decoded_progress =
        decoded ? std::get_if<contracts::TaskProgress>(&decoded.value().payload) : nullptr;
    bool passed = expect(decoded_progress != nullptr && decoded.value().sequence == 7 &&
                             decoded_progress->processed_bytes == 50,
                         "task progress event roundtrips through the V3 codec");

    contracts::TaskResult result;
    result.job_id = "job:backup-1";
    result.trace_id = "trace:backup-1";
    result.outcome = contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.message_code = "backup.completed";
    event.kind = contracts::ServiceEventKind::kTaskCompleted;
    event.sequence = 8;
    event.message_code = "backup.completed";
    event.payload = result;
    encoded = app::encode_service_event(event);
    decoded = encoded ? app::decode_service_event(encoded.value()) : app::decode_service_event({});
    passed &=
        expect(decoded && std::holds_alternative<contracts::TaskResult>(decoded.value().payload),
               "task completion event roundtrips");

    contracts::MountSessionSummary mount{
        "mount:1", "recovery:1", contracts::MountSessionState::kMounted, "M:", 1, "mount.ready"};
    event.kind = contracts::ServiceEventKind::kMountSessionChanged;
    event.sequence = 9;
    event.message_code = "mount.ready";
    event.payload = mount;
    encoded = app::encode_service_event(event);
    decoded = encoded ? app::decode_service_event(encoded.value()) : app::decode_service_event({});
    passed &= expect(
        decoded && std::holds_alternative<contracts::MountSessionSummary>(decoded.value().payload),
        "mount session event roundtrips");
    return passed;
}

bool test_structured_rejections() {
    auto invalid = app::handle_service_message("{}", runtime_info());
    auto invalid_response =
        invalid ? app::decode_service_response(invalid.value()) : app::decode_service_response({});
    bool passed = expect(
        invalid_response &&
            invalid_response.value().kind == contracts::ServiceResponseKind::kRequestFailed &&
            invalid_response.value().boundary_error_code == base::ErrorCode::kInvalidArgument,
        "malformed request returns a structured rejection");
    auto unsupported = app::handle_service_message(
        R"({"schema_version":2,"message_type":1,"request_id":"request-2","kind":1,"idempotency_key":null,"payload":{"minimum_api_version":3,"maximum_api_version":3}})",
        runtime_info());
    auto unsupported_response = unsupported ? app::decode_service_response(unsupported.value())
                                            : app::decode_service_response({});
    passed &= expect(unsupported_response && unsupported_response.value().boundary_error_code ==
                                                 base::ErrorCode::kUnsupportedVersion,
                     "unsupported schema remains distinguishable at the process boundary");
    return passed;
}

class MemoryChannel final : public aegra::ports::IMessageChannel {
  public:
    explicit MemoryChannel(std::string request) : request_(std::move(request)) {}

    base::Result<std::string> receive(const base::CancellationToken&) override {
        if (received_) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kIoFailure, "test channel is exhausted"});
        }
        received_ = true;
        return base::Result<std::string>::success(request_);
    }

    base::Result<void> send(const std::string_view message,
                            const base::CancellationToken&) override {
        response_ = message;
        return base::Result<void>::success();
    }

    [[nodiscard]] const std::string& response() const noexcept { return response_; }

  private:
    std::string request_;
    std::string response_;
    bool received_{false};
};

bool test_session() {
    auto request = app::encode_service_request(service_request());
    if (!expect(request.has_value(), "session request encodes")) {
        return false;
    }
    MemoryChannel channel(request.value());
    auto result = app::run_service_session(channel, runtime_info(), {}, 1);
    auto response = app::decode_service_response(channel.response());
    return expect(result.has_value(), "one-request service session completes") &&
           expect(response && response.value().request_id == "request-1",
                  "service session preserves request correlation");
}

int run_tests() {
    return test_request_codec() && test_response_codec_and_dispatch() &&
                   test_repository_list_codec_and_dispatch() &&
                   test_command_codec_and_unavailable_dispatch() &&
                   test_disabled_s5_capabilities_block_injected_handlers() &&
                   test_planned_request_payload_codecs() &&
                   test_planned_response_payload_codecs() && test_command_acknowledgement_codec() &&
                   test_event_codec() && test_structured_rejections() && test_session()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
