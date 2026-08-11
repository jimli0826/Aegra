#include "aegra/apps/service/service_protocol.h"

#include "service_protocol_json.h"

#include <array>
#include <exception>
#include <stdexcept>
#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

using protocol_json::Json;

[[nodiscard]] base::Error invalid_protocol(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] contracts::ServiceRequest parse_request(const Json& root) {
    constexpr std::array<std::string_view, 6> keys{
        "schema_version", "message_type", "request_id", "kind", "idempotency_key", "payload"};
    if (!protocol_json::exact_keys(root, keys)) {
        throw std::invalid_argument("service request fields are invalid");
    }
    contracts::ServiceRequest request;
    request.schema_version = protocol_json::unsigned_value<std::uint32_t>(root, "schema_version");
    request.message_type = static_cast<contracts::ServiceMessageType>(
        protocol_json::unsigned_value<std::uint8_t>(root, "message_type"));
    request.request_id = root.at("request_id").get<std::string>();
    request.kind = static_cast<contracts::ServiceRequestKind>(
        protocol_json::unsigned_value<std::uint8_t>(root, "kind"));
    request.idempotency_key = protocol_json::optional_string(root.at("idempotency_key"));
    request.payload = protocol_json::parse_request_payload(request.kind, root.at("payload"));
    return request;
}

[[nodiscard]] Json encode_request_object(const contracts::ServiceRequest& request) {
    return Json{{"schema_version", request.schema_version},
                {"message_type", static_cast<std::uint8_t>(request.message_type)},
                {"request_id", request.request_id},
                {"kind", static_cast<std::uint8_t>(request.kind)},
                {"idempotency_key", protocol_json::optional_string_json(request.idempotency_key)},
                {"payload", protocol_json::encode_request_payload(request)}};
}

[[nodiscard]] contracts::ServiceResponse parse_response(const Json& root) {
    constexpr std::array<std::string_view, 9> keys{
        "schema_version",      "message_type", "request_id",        "kind",   "request_kind",
        "boundary_error_code", "message_code", "message_arguments", "payload"};
    if (!protocol_json::exact_keys(root, keys)) {
        throw std::invalid_argument("service response fields are invalid");
    }
    contracts::ServiceResponse response;
    response.schema_version = protocol_json::unsigned_value<std::uint32_t>(root, "schema_version");
    response.message_type = static_cast<contracts::ServiceMessageType>(
        protocol_json::unsigned_value<std::uint8_t>(root, "message_type"));
    response.request_id = root.at("request_id").get<std::string>();
    response.kind = static_cast<contracts::ServiceResponseKind>(
        protocol_json::unsigned_value<std::uint8_t>(root, "kind"));
    response.request_kind = static_cast<contracts::ServiceRequestKind>(
        protocol_json::unsigned_value<std::uint8_t>(root, "request_kind"));
    response.boundary_error_code = static_cast<base::ErrorCode>(
        protocol_json::unsigned_value<std::uint32_t>(root, "boundary_error_code"));
    response.message_code = root.at("message_code").get<std::string>();
    response.message_arguments =
        protocol_json::parse_message_arguments(root.at("message_arguments"));
    response.payload = protocol_json::parse_response_payload(response.kind, response.request_kind,
                                                             root.at("payload"));
    return response;
}

[[nodiscard]] Json encode_response_object(const contracts::ServiceResponse& response) {
    return Json{
        {"schema_version", response.schema_version},
        {"message_type", static_cast<std::uint8_t>(response.message_type)},
        {"request_id", response.request_id},
        {"kind", static_cast<std::uint8_t>(response.kind)},
        {"request_kind", static_cast<std::uint8_t>(response.request_kind)},
        {"boundary_error_code", static_cast<std::uint32_t>(response.boundary_error_code)},
        {"message_code", response.message_code},
        {"message_arguments", protocol_json::encode_message_arguments(response.message_arguments)},
        {"payload", protocol_json::encode_response_payload(response)}};
}

[[nodiscard]] contracts::ServiceEvent parse_event(const Json& root) {
    constexpr std::array<std::string_view, 8> keys{
        "schema_version", "message_type", "subscription_id",   "sequence",
        "kind",           "message_code", "message_arguments", "payload"};
    if (!protocol_json::exact_keys(root, keys)) {
        throw std::invalid_argument("service event fields are invalid");
    }
    contracts::ServiceEvent event;
    event.schema_version = protocol_json::unsigned_value<std::uint32_t>(root, "schema_version");
    event.message_type = static_cast<contracts::ServiceMessageType>(
        protocol_json::unsigned_value<std::uint8_t>(root, "message_type"));
    event.subscription_id = root.at("subscription_id").get<std::string>();
    event.sequence = protocol_json::unsigned_value<std::uint64_t>(root, "sequence");
    event.kind = static_cast<contracts::ServiceEventKind>(
        protocol_json::unsigned_value<std::uint8_t>(root, "kind"));
    event.message_code = root.at("message_code").get<std::string>();
    event.message_arguments = protocol_json::parse_message_arguments(root.at("message_arguments"));
    event.payload = protocol_json::parse_event_payload(event.kind, root.at("payload"));
    return event;
}

[[nodiscard]] Json encode_event_object(const contracts::ServiceEvent& event) {
    return Json{
        {"schema_version", event.schema_version},
        {"message_type", static_cast<std::uint8_t>(event.message_type)},
        {"subscription_id", event.subscription_id},
        {"sequence", event.sequence},
        {"kind", static_cast<std::uint8_t>(event.kind)},
        {"message_code", event.message_code},
        {"message_arguments", protocol_json::encode_message_arguments(event.message_arguments)},
        {"payload", protocol_json::encode_event_payload(event)}};
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

template <typename Value, typename Encoder, typename Validator>
[[nodiscard]] base::Result<std::string> encode(const Value& value, Encoder encoder,
                                               Validator validator, const char* failure_message) {
    auto valid = validator(value);
    if (!valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    try {
        auto encoded = encoder(value).dump();
        if (encoded.empty() || encoded.size() > kMaximumServiceFrameBytes) {
            return base::Result<std::string>::failure(invalid_protocol(
                base::ErrorCode::kInvalidArgument,
                "service message exceeds maximum frame bytes"));
        }
        return base::Result<std::string>::success(std::move(encoded));
    } catch (const std::exception&) {
        return base::Result<std::string>::failure(
            invalid_protocol(base::ErrorCode::kInternal, failure_message));
    }
}

} // namespace

base::Result<std::string> encode_service_request(const contracts::ServiceRequest& request) {
    return encode(request, encode_request_object, contracts::validate_service_request,
                  "service request encoding failed");
}

base::Result<contracts::ServiceRequest> decode_service_request(const std::string_view encoded) {
    return decode<contracts::ServiceRequest>(encoded, parse_request,
                                             contracts::validate_service_request,
                                             "service request JSON is invalid");
}

base::Result<std::string> encode_service_response(const contracts::ServiceResponse& response) {
    return encode(response, encode_response_object, contracts::validate_service_response,
                  "service response encoding failed");
}

base::Result<contracts::ServiceResponse> decode_service_response(const std::string_view encoded) {
    return decode<contracts::ServiceResponse>(encoded, parse_response,
                                              contracts::validate_service_response,
                                              "service response JSON is invalid");
}

base::Result<std::string> encode_service_event(const contracts::ServiceEvent& event) {
    return encode(event, encode_event_object, contracts::validate_service_event,
                  "service event encoding failed");
}

base::Result<contracts::ServiceEvent> decode_service_event(const std::string_view encoded) {
    return decode<contracts::ServiceEvent>(encoded, parse_event, contracts::validate_service_event,
                                           "service event JSON is invalid");
}

} // namespace aegra::apps::service
