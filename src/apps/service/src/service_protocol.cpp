#include "aegra/apps/service/service_protocol.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using Json = nlohmann::json;

template <std::size_t Size>
[[nodiscard]] bool exact_keys(const Json& object, const std::array<std::string_view, Size>& keys) {
    if (!object.is_object() || object.size() != Size) {
        return false;
    }
    for (const auto key : keys) {
        if (!object.contains(key)) {
            return false;
        }
    }
    return true;
}

template <typename Integer>
[[nodiscard]] Integer unsigned_value(const Json& object, const char* key) {
    const auto& value = object.at(key);
    if (!value.is_number_unsigned()) {
        throw std::invalid_argument("service protocol integer is invalid");
    }
    const auto decoded = value.get<std::uint64_t>();
    if (decoded > static_cast<std::uint64_t>((std::numeric_limits<Integer>::max)())) {
        throw std::out_of_range("service protocol integer is out of range");
    }
    return static_cast<Integer>(decoded);
}

[[nodiscard]] base::Error invalid_protocol(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] contracts::ServiceRequest parse_request(const Json& root) {
    constexpr std::array<std::string_view, 3> keys{"schema_version", "request_id", "kind"};
    if (!exact_keys(root, keys)) {
        throw std::invalid_argument("service request fields are invalid");
    }
    contracts::ServiceRequest request;
    request.schema_version = unsigned_value<std::uint32_t>(root, "schema_version");
    request.request_id = root.at("request_id").get<std::string>();
    request.kind =
        static_cast<contracts::ServiceRequestKind>(unsigned_value<std::uint8_t>(root, "kind"));
    return request;
}

[[nodiscard]] Json encode_service_info(const contracts::ServiceInfo& service) {
    return Json{
        {"api_version", service.api_version},
        {"state", static_cast<std::uint8_t>(service.state)},
        {"service_version", service.service_version},
        {"capabilities", service.capabilities},
    };
}

[[nodiscard]] contracts::ServiceInfo parse_service_info(const Json& root) {
    constexpr std::array<std::string_view, 4> keys{"api_version", "state", "service_version",
                                                   "capabilities"};
    if (!exact_keys(root, keys)) {
        throw std::invalid_argument("service info fields are invalid");
    }
    contracts::ServiceInfo service;
    service.api_version = unsigned_value<std::uint32_t>(root, "api_version");
    service.state =
        static_cast<contracts::ServiceState>(unsigned_value<std::uint8_t>(root, "state"));
    service.service_version = root.at("service_version").get<std::string>();
    service.capabilities = root.at("capabilities").get<std::vector<std::string>>();
    return service;
}

[[nodiscard]] Json encode_response_object(const contracts::ServiceResponse& response) {
    Json root{
        {"schema_version", response.schema_version},
        {"request_id", response.request_id},
        {"kind", static_cast<std::uint8_t>(response.kind)},
        {"boundary_error_code", static_cast<std::uint32_t>(response.boundary_error_code)},
        {"message_code", response.message_code},
    };
    root["service"] = response.service ? encode_service_info(*response.service) : Json(nullptr);
    return root;
}

[[nodiscard]] contracts::ServiceResponse parse_response(const Json& root) {
    constexpr std::array<std::string_view, 6> keys{
        "schema_version", "request_id", "kind", "boundary_error_code", "message_code", "service"};
    if (!exact_keys(root, keys)) {
        throw std::invalid_argument("service response fields are invalid");
    }
    contracts::ServiceResponse response;
    response.schema_version = unsigned_value<std::uint32_t>(root, "schema_version");
    response.request_id = root.at("request_id").get<std::string>();
    response.kind =
        static_cast<contracts::ServiceResponseKind>(unsigned_value<std::uint8_t>(root, "kind"));
    response.boundary_error_code =
        static_cast<base::ErrorCode>(unsigned_value<std::uint32_t>(root, "boundary_error_code"));
    response.message_code = root.at("message_code").get<std::string>();
    if (!root.at("service").is_null()) {
        response.service = parse_service_info(root.at("service"));
    }
    return response;
}

template <typename Value, typename Parser, typename Validator>
[[nodiscard]] base::Result<Value> decode(std::string_view encoded, Parser parser,
                                         Validator validator, const char* invalid_message) {
    if (encoded.empty() || encoded.size() > kMaximumServiceFrameBytes) {
        return base::Result<Value>::failure(
            invalid_protocol(base::ErrorCode::kInvalidArgument, invalid_message));
    }
    try {
        auto value = parser(Json::parse(encoded));
        auto valid = validator(value);
        return valid ? base::Result<Value>::success(std::move(value))
                     : base::Result<Value>::failure(valid.error());
    } catch (const std::exception&) {
        return base::Result<Value>::failure(
            invalid_protocol(base::ErrorCode::kInvalidArgument, invalid_message));
    }
}

} // namespace

base::Result<std::string> encode_service_request(const contracts::ServiceRequest& request) {
    auto valid = contracts::validate_service_request(request);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    try {
        return base::Result<std::string>::success(Json{
            {"schema_version", request.schema_version},
            {"request_id", request.request_id},
            {"kind",
             static_cast<std::uint8_t>(request.kind)}}.dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            invalid_protocol(base::ErrorCode::kInternal, "service request encoding failed"));
    }
}

base::Result<contracts::ServiceRequest> decode_service_request(const std::string_view encoded) {
    return decode<contracts::ServiceRequest>(encoded, parse_request,
                                             contracts::validate_service_request,
                                             "service request JSON is invalid");
}

base::Result<std::string> encode_service_response(const contracts::ServiceResponse& response) {
    auto valid = contracts::validate_service_response(response);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    try {
        return base::Result<std::string>::success(encode_response_object(response).dump());
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            invalid_protocol(base::ErrorCode::kInternal, "service response encoding failed"));
    }
}

base::Result<contracts::ServiceResponse> decode_service_response(const std::string_view encoded) {
    return decode<contracts::ServiceResponse>(encoded, parse_response,
                                              contracts::validate_service_response,
                                              "service response JSON is invalid");
}

} // namespace aegra::apps::service
