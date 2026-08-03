#include "aegra/contracts/service.h"

#include "aegra/base/error.h"

#include <cstdio>
#include <cstdlib>

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
                     "non-ready service info is rejected by schema 1");
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
    contracts::ServiceResponse rejection;
    rejection.kind = contracts::ServiceResponseKind::kRequestRejected;
    rejection.boundary_error_code = base::ErrorCode::kInvalidArgument;
    rejection.message_code = "service.request_rejected";
    passed &= expect(contracts::validate_service_response(rejection).has_value(),
                     "request rejection may omit an untrusted request id");
    rejection.service = ready_service();
    passed &= expect(!contracts::validate_service_response(rejection),
                     "request rejection cannot carry service info");
    rejection.service.reset();
    rejection.boundary_error_code = base::ErrorCode::kInternal;
    passed &= expect(!contracts::validate_service_response(rejection),
                     "request rejection only exposes stable client errors");
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
