#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"

#include "aegra/base/error.h"
#include "aegra/contracts/service.h"
#include "aegra/ports/message_channel.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
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

contracts::ServiceRequest service_request() {
    return {.schema_version = contracts::kServiceRequestSchemaVersion,
            .request_id = "request-1",
            .kind = contracts::ServiceRequestKind::kGetServiceInfo};
}

bool test_request_codec() {
    auto encoded = app::encode_service_request(service_request());
    bool passed = expect(encoded && encoded.value() ==
                                        R"({"kind":1,"request_id":"request-1","schema_version":2})",
                         "service request encoding has a stable schema");
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    passed &=
        expect(decoded && decoded.value().request_id == "request-1", "service request roundtrips");
    passed &= expect(!app::decode_service_request(
                         R"({"schema_version":2,"request_id":"request-1","kind":1,"extra":0})"),
                     "unknown request fields are rejected");
    passed &= expect(
        !app::decode_service_request(R"({"schema_version":2,"request_id":"request-1","kind":"1"})"),
        "wrong request field types are rejected");
    passed &= expect(!app::decode_service_request("{"), "malformed request JSON is rejected");
    const std::string oversized(app::kMaximumServiceFrameBytes + 1, 'x');
    passed &= expect(!app::decode_service_request(oversized),
                     "oversized request is rejected before parsing");
    return passed;
}

bool test_response_codec_and_dispatch() {
    auto dispatched = app::dispatch_service_request(service_request(), runtime_info());
    bool passed = expect(dispatched && dispatched.value().service &&
                             dispatched.value().service->service_version == "0.1.0",
                         "GetServiceInfo dispatches to owned runtime data");
    auto encoded = dispatched ? app::encode_service_response(dispatched.value())
                              : app::encode_service_response({});
    auto decoded =
        encoded ? app::decode_service_response(encoded.value()) : app::decode_service_response({});
    passed &=
        expect(decoded && decoded.value().request_id == "request-1" && decoded.value().service &&
                   decoded.value().service->capabilities ==
                       std::vector<std::string>{"repository.list", "service.info"},
               "service response roundtrips with stable capability data");
    auto invalid_runtime = runtime_info();
    invalid_runtime.capabilities = {"service.info", "repository.list"};
    passed &= expect(!app::dispatch_service_request(service_request(), invalid_runtime),
                     "invalid runtime capabilities cannot cross the boundary");
    return passed;
}

bool test_repository_list_codec_and_dispatch() {
    contracts::ServiceRequest request;
    request.request_id = "repository-request-1";
    request.kind = contracts::ServiceRequestKind::kListRecoveryPoints;
    request.repository_list = contracts::RecoveryPointListRequest{25, std::nullopt};
    auto encoded = app::encode_service_request(request);
    bool passed =
        expect(encoded && encoded.value().find("\"repository_list\"") != std::string::npos &&
                   encoded.value().find("\"maximum_results\":25") != std::string::npos,
               "repository list request encodes its bounded page payload");
    auto decoded =
        encoded ? app::decode_service_request(encoded.value()) : app::decode_service_request({});
    passed &= expect(decoded && decoded.value().repository_list &&
                         decoded.value().repository_list->maximum_results == 25,
                     "repository list request roundtrips");
    auto response = decoded ? app::dispatch_service_request(decoded.value(), runtime_info())
                            : app::dispatch_service_request({}, runtime_info());
    passed &= expect(response && response.value().recovery_points &&
                         response.value().recovery_points->state ==
                             contracts::RepositoryCatalogState::kNotConfigured,
                     "unconfigured Service returns a valid empty repository page");
    auto response_json = response ? app::encode_service_response(response.value())
                                  : app::encode_service_response({});
    auto roundtrip = response_json ? app::decode_service_response(response_json.value())
                                   : app::decode_service_response({});
    passed &= expect(roundtrip && roundtrip.value().recovery_points &&
                         roundtrip.value().request_id == "repository-request-1",
                     "repository page response roundtrips with request correlation");
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
        R"({"schema_version":1,"request_id":"request-2","kind":1})", runtime_info());
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
                   test_repository_list_codec_and_dispatch() && test_structured_rejections() &&
                   test_session()
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
