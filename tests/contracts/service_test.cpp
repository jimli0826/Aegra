#include "aegra/contracts/service.h"

#include "aegra/base/error.h"

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>

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
    return {
        .api_version = contracts::kServiceApiVersion,
        .state = contracts::ServiceState::kReady,
        .service_version = "0.1.0",
        .capabilities = {"repository.list", "service.info"},
    };
}

contracts::ServiceResponse ready_response() {
    return {
        .schema_version = contracts::kServiceResponseSchemaVersion,
        .request_id = "request-1",
        .kind = contracts::ServiceResponseKind::kServiceInfo,
        .boundary_error_code = base::ErrorCode::kNone,
        .message_code = "service.ready",
        .service = ready_service(),
    };
}

bool test_request_validation() {
    contracts::ServiceRequest request;
    request.request_id = "request-1";
    bool passed = expect(contracts::validate_service_request(request).has_value(),
                         "valid service request is accepted");
    request.schema_version = 99;
    auto unsupported = contracts::validate_service_request(request);
    passed &=
        expect(!unsupported && unsupported.error().code == base::ErrorCode::kUnsupportedVersion,
               "unknown request schema is rejected as unsupported");
    request.schema_version = contracts::kServiceRequestSchemaVersion;
    request.request_id.clear();
    passed &= expect(!contracts::validate_service_request(request), "empty request id is rejected");
    request.request_id = "request 1";
    passed &= expect(!contracts::validate_service_request(request),
                     "request id with whitespace is rejected");
    request.request_id = "request-1";
    request.kind = contracts::ServiceRequestKind::kListRecoveryPoints;
    request.repository_list = contracts::RecoveryPointListRequest{25, std::nullopt};
    passed &= expect(contracts::validate_service_request(request).has_value(),
                     "valid recovery point list request is accepted");
    request.repository_list->maximum_results = 0;
    passed &= expect(!contracts::validate_service_request(request),
                     "zero-sized recovery point page is rejected");
    request.repository_list.reset();
    request.kind = static_cast<contracts::ServiceRequestKind>(255);
    passed &=
        expect(!contracts::validate_service_request(request), "unknown request kind is rejected");
    return passed;
}

bool test_service_info_validation() {
    auto service = ready_service();
    bool passed = expect(contracts::validate_service_info(service).has_value(),
                         "valid ready service info is accepted");
    service.state = contracts::ServiceState::kStarting;
    passed &= expect(!contracts::validate_service_info(service),
                     "non-ready service info is rejected by schema 2");
    service = ready_service();
    service.capabilities = {"service.info", "repository.list"};
    passed &=
        expect(!contracts::validate_service_info(service), "unsorted capabilities are rejected");
    service.capabilities = {"service.info", "service.info"};
    passed &=
        expect(!contracts::validate_service_info(service), "duplicate capabilities are rejected");
    service.capabilities = {"Service.Info"};
    passed &= expect(!contracts::validate_service_info(service),
                     "unstable capability characters are rejected");
    return passed;
}

bool test_response_validation() {
    auto response = ready_response();
    bool passed = expect(contracts::validate_service_response(response).has_value(),
                         "valid service response is accepted");
    response.boundary_error_code = base::ErrorCode::kInternal;
    passed &= expect(!contracts::validate_service_response(response),
                     "successful response cannot carry a boundary error");
    contracts::ServiceResponse failure;
    failure.kind = contracts::ServiceResponseKind::kRequestFailed;
    failure.boundary_error_code = base::ErrorCode::kInternal;
    failure.message_code = "repository.query_failed";
    passed &= expect(contracts::validate_service_response(failure).has_value(),
                     "request failure may omit an untrusted request id");
    failure.service = ready_service();
    passed &= expect(!contracts::validate_service_response(failure),
                     "request failure cannot carry service info");

    contracts::ServiceResponse page;
    page.request_id = "request-2";
    page.kind = contracts::ServiceResponseKind::kRecoveryPointPage;
    page.boundary_error_code = base::ErrorCode::kNone;
    page.message_code = "repository.not_configured";
    page.recovery_points = contracts::RecoveryPointPage{};
    passed &= expect(contracts::validate_service_response(page).has_value(),
                     "not-configured recovery point page is valid");
    page.service = ready_service();
    passed &= expect(!contracts::validate_service_response(page),
                     "response payload variants are mutually exclusive");
    contracts::RecoveryPointSummary summary;
    summary.file_uuid = "11111111-2222-4333-8444-555555555555";
    summary.backup_set_uuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
    summary.chain_state = contracts::RecoveryPointChainState::kComplete;
    summary.logical_size_bytes = (std::numeric_limits<std::uint64_t>::max)();
    passed &= expect(!contracts::validate_recovery_point_summary(summary),
                     "wire integers larger than signed 64-bit are rejected");
    return passed;
}

int run_tests() {
    return test_request_validation() && test_service_info_validation() && test_response_validation()
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
