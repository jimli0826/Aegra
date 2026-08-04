#include "aegra/contracts/service.h"

#include "aegra/base/error.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <variant>

namespace {

namespace base = aegra::base;
namespace contracts = aegra::contracts;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

contracts::ServiceInfo ready_service() {
    return {.minimum_api_version = contracts::kServiceApiVersion,
            .api_version = contracts::kServiceApiVersion,
            .state = contracts::ServiceState::kReady,
            .service_version = "0.1.0",
            .capabilities = {"repository.list", "service.info"}};
}

contracts::ServiceResponse ready_response() {
    return {.schema_version = contracts::kServiceResponseSchemaVersion,
            .message_type = contracts::ServiceMessageType::kResponse,
            .request_id = "request-1",
            .kind = contracts::ServiceResponseKind::kQueryResult,
            .request_kind = contracts::ServiceRequestKind::kGetServiceInfo,
            .boundary_error_code = base::ErrorCode::kNone,
            .message_code = "service.ready",
            .payload = ready_service()};
}

bool test_query_request_validation() {
    contracts::ServiceRequest request;
    request.request_id = "request-1";
    bool passed = expect(contracts::validate_service_request(request).has_value(),
                         "valid V3 service info query is accepted");
    request.schema_version = 2;
    auto unsupported = contracts::validate_service_request(request);
    passed &=
        expect(!unsupported && unsupported.error().code == base::ErrorCode::kUnsupportedVersion,
               "unreleased schema 2 is rejected without compatibility");
    request.schema_version = contracts::kServiceRequestSchemaVersion;
    request.message_type = contracts::ServiceMessageType::kEvent;
    passed &= expect(!contracts::validate_service_request(request),
                     "request envelope rejects an event message type");
    request.message_type = contracts::ServiceMessageType::kRequest;
    request.idempotency_key = "query-key";
    passed &=
        expect(!contracts::validate_service_request(request), "query rejects an idempotency key");

    request.idempotency_key.reset();
    request.kind = contracts::ServiceRequestKind::kListRecoveryPoints;
    request.payload = contracts::ServiceRecoveryPointListRequest{std::nullopt, {25, std::nullopt}};
    passed &= expect(contracts::validate_service_request(request).has_value(),
                     "recovery point page query is accepted");
    request.payload = contracts::SourceInventoryListRequest{{25, std::nullopt}, true};
    passed &= expect(!contracts::validate_service_request(request),
                     "query kind and payload must match exactly");
    return passed;
}

bool test_command_request_validation() {
    contracts::ServiceRequest request;
    request.request_id = "request-2";
    request.kind = contracts::ServiceRequestKind::kStartBackup;
    request.payload = contracts::StartBackupCommand{
        .source_id = "source:volume-1",
        .repository_connection_id = "repository:primary",
        .backup_type = contracts::BackupType::kFull,
    };
    bool passed = expect(!contracts::validate_service_request(request),
                         "command requires an idempotency key");
    request.idempotency_key = "backup-command-1";
    passed &= expect(contracts::validate_service_request(request).has_value(),
                     "valid idempotent backup command is accepted");
    auto& backup = std::get<contracts::StartBackupCommand>(request.payload);
    backup.backup_type = contracts::BackupType::kIncremental;
    passed &= expect(!contracts::validate_service_request(request),
                     "incremental backup requires an explicit parent");
    backup.parent_recovery_point_id = "recovery:parent-1";
    passed &= expect(contracts::validate_service_request(request).has_value(),
                     "incremental backup with parent is accepted");
    request.kind = static_cast<contracts::ServiceRequestKind>(255);
    passed &=
        expect(!contracts::validate_service_request(request), "unknown request kind is rejected");

    contracts::ServiceRequest restore;
    restore.request_id = "request-restore";
    restore.kind = contracts::ServiceRequestKind::kStartRestore;
    restore.idempotency_key = "restore-command-1";
    restore.payload = contracts::StartRestoreCommand{"preflight-token", false};
    passed &= expect(!contracts::validate_service_request(restore),
                     "restore start requires explicit confirmation");
    std::get<contracts::StartRestoreCommand>(restore.payload).confirmed = true;
    passed &= expect(contracts::validate_service_request(restore).has_value(),
                     "confirmed restore start is accepted");
    return passed;
}

bool test_restore_preflight_validation() {
    contracts::RestorePreflightRequest request{"repository:1", "recovery:1", "source:target-1"};
    bool passed = expect(contracts::validate_restore_preflight_request(request).has_value(),
                         "restore preflight request owns repository identity");
    request.repository_connection_id.clear();
    passed &= expect(!contracts::validate_restore_preflight_request(request),
                     "restore preflight rejects missing repository identity");

    contracts::RestorePreflight preflight{"preflight-token",
                                          "repository:1",
                                          "recovery:1",
                                          "source:target-1",
                                          100,
                                          200,
                                          1,
                                          1'800'000'000'000ULL,
                                          true,
                                          "restore.preflight_ready"};
    passed &= expect(contracts::validate_restore_preflight(preflight).has_value(),
                     "eligible restore preflight is accepted");
    preflight.target_capacity_bytes = 99;
    passed &= expect(!contracts::validate_restore_preflight(preflight),
                     "restore preflight rejects an undersized target");
    return passed;
}

bool test_service_info_validation() {
    auto service = ready_service();
    bool passed = expect(contracts::validate_service_info(service).has_value(),
                         "valid service info is accepted");
    service.state = contracts::ServiceState::kStarting;
    passed &= expect(contracts::validate_service_info(service).has_value(),
                     "V3 represents the starting state");
    service = ready_service();
    service.capabilities = {"service.info", "repository.list"};
    passed &=
        expect(!contracts::validate_service_info(service), "unsorted capabilities are rejected");
    service = ready_service();
    service.minimum_api_version = service.api_version + 1;
    passed &=
        expect(!contracts::validate_service_info(service), "invalid service API range is rejected");
    return passed;
}

bool test_response_validation() {
    auto response = ready_response();
    bool passed = expect(contracts::validate_service_response(response).has_value(),
                         "valid query result is accepted");
    response.payload = contracts::ServiceRecoveryPointPage{};
    passed &= expect(!contracts::validate_service_response(response),
                     "query response payload must match request kind");

    contracts::ServiceResponse failure;
    failure.kind = contracts::ServiceResponseKind::kRequestFailed;
    failure.request_kind = contracts::ServiceRequestKind::kStartBackup;
    failure.boundary_error_code = base::ErrorCode::kConflict;
    failure.message_code = "service.capability_unavailable";
    failure.message_arguments = {{"request_kind", "37"}};
    passed &= expect(contracts::validate_service_response(failure).has_value(),
                     "structured failure may omit an untrusted request id");

    contracts::ServiceResponse accepted;
    accepted.request_id = "request-3";
    accepted.kind = contracts::ServiceResponseKind::kCommandAccepted;
    accepted.request_kind = contracts::ServiceRequestKind::kStartBackup;
    accepted.boundary_error_code = base::ErrorCode::kNone;
    accepted.message_code = "backup.accepted";
    accepted.payload =
        contracts::CommandAcknowledgement{.command_id = "command:backup-1",
                                          .disposition = contracts::CommandDisposition::kAccepted,
                                          .resource_id = "job:backup-1"};
    passed &= expect(contracts::validate_service_response(accepted).has_value(),
                     "command acknowledgement is accepted");
    return passed;
}

bool test_event_validation() {
    contracts::TaskProgress progress;
    progress.job_id = "job:backup-1";
    progress.trace_id = "trace:backup-1";
    progress.phase = contracts::TaskPhase::kReading;
    progress.logical_bytes = 100;
    progress.processed_bytes = 25;
    progress.message_code = "backup.reading";

    contracts::ServiceEvent event;
    event.subscription_id = "subscription:desktop-1";
    event.sequence = 1;
    event.message_code = "backup.reading";
    event.payload = progress;
    bool passed = expect(contracts::validate_service_event(event).has_value(),
                         "correlated task progress event is accepted");
    event.sequence = 0;
    passed &= expect(!contracts::validate_service_event(event), "event sequence zero is rejected");
    event.sequence = 1;
    event.kind = contracts::ServiceEventKind::kTaskCompleted;
    passed &=
        expect(!contracts::validate_service_event(event), "event kind and payload must match");
    return passed;
}

int run_tests() {
    return test_query_request_validation() && test_command_request_validation() &&
                   test_restore_preflight_validation() && test_service_info_validation() &&
                   test_response_validation() && test_event_validation()
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
