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

[[nodiscard]] std::optional<std::string> optional_string(const Json& value) {
    if (value.is_null()) {
        return std::nullopt;
    }
    if (!value.is_string()) {
        throw std::invalid_argument("service protocol optional string is invalid");
    }
    return value.get<std::string>();
}

[[nodiscard]] contracts::RecoveryPointListRequest parse_repository_list(const Json& root) {
    constexpr std::array<std::string_view, 2> keys{"maximum_results", "continuation_token"};
    if (!exact_keys(root, keys)) {
        throw std::invalid_argument("repository list request fields are invalid");
    }
    contracts::RecoveryPointListRequest request;
    request.maximum_results = unsigned_value<std::uint32_t>(root, "maximum_results");
    request.continuation_token = optional_string(root.at("continuation_token"));
    return request;
}

[[nodiscard]] contracts::ServiceRequest parse_request(const Json& root) {
    constexpr std::array<std::string_view, 3> info_keys{"schema_version", "request_id", "kind"};
    constexpr std::array<std::string_view, 4> list_keys{"schema_version", "request_id", "kind",
                                                        "repository_list"};
    if (!root.is_object() || !root.contains("kind")) {
        throw std::invalid_argument("service request fields are invalid");
    }
    contracts::ServiceRequest request;
    request.schema_version = unsigned_value<std::uint32_t>(root, "schema_version");
    request.request_id = root.at("request_id").get<std::string>();
    request.kind =
        static_cast<contracts::ServiceRequestKind>(unsigned_value<std::uint8_t>(root, "kind"));
    if (request.kind == contracts::ServiceRequestKind::kListRecoveryPoints) {
        if (!exact_keys(root, list_keys)) {
            throw std::invalid_argument("repository list root fields are invalid");
        }
        request.repository_list = parse_repository_list(root.at("repository_list"));
    } else if (!exact_keys(root, info_keys)) {
        throw std::invalid_argument("service request root fields are invalid");
    }
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

[[nodiscard]] Json encode_recovery_point(const contracts::RecoveryPointSummary& point) {
    return Json{
        {"file_uuid", point.file_uuid},
        {"backup_set_uuid", point.backup_set_uuid},
        {"parent_uuid", point.parent_uuid ? Json(*point.parent_uuid) : Json(nullptr)},
        {"backup_type", static_cast<std::uint8_t>(point.backup_type)},
        {"chain_state", static_cast<std::uint8_t>(point.chain_state)},
        {"created_utc_ms", point.created_utc_ms},
        {"logical_size_bytes", point.logical_size_bytes},
        {"stored_size_bytes", point.stored_size_bytes},
        {"source_count", point.source_count},
        {"has_sidecar", point.has_sidecar},
    };
}

[[nodiscard]] Json encode_recovery_point_page(const contracts::RecoveryPointPage& page) {
    Json items = Json::array();
    for (const auto& item : page.items) {
        items.push_back(encode_recovery_point(item));
    }
    return Json{
        {"state", static_cast<std::uint8_t>(page.state)},
        {"repository_uuid", page.repository_uuid},
        {"items", std::move(items)},
        {"continuation_token",
         page.continuation_token ? Json(*page.continuation_token) : Json(nullptr)},
    };
}

[[nodiscard]] contracts::RecoveryPointSummary parse_recovery_point(const Json& root) {
    constexpr std::array<std::string_view, 10> keys{
        "file_uuid",      "backup_set_uuid",    "parent_uuid",       "backup_type",  "chain_state",
        "created_utc_ms", "logical_size_bytes", "stored_size_bytes", "source_count", "has_sidecar",
    };
    if (!exact_keys(root, keys)) {
        throw std::invalid_argument("recovery point summary fields are invalid");
    }
    contracts::RecoveryPointSummary point;
    point.file_uuid = root.at("file_uuid").get<std::string>();
    point.backup_set_uuid = root.at("backup_set_uuid").get<std::string>();
    point.parent_uuid = optional_string(root.at("parent_uuid"));
    point.backup_type = static_cast<contracts::PersonalBackupType>(
        unsigned_value<std::uint8_t>(root, "backup_type"));
    point.chain_state = static_cast<contracts::RecoveryPointChainState>(
        unsigned_value<std::uint8_t>(root, "chain_state"));
    point.created_utc_ms = unsigned_value<std::uint64_t>(root, "created_utc_ms");
    point.logical_size_bytes = unsigned_value<std::uint64_t>(root, "logical_size_bytes");
    point.stored_size_bytes = unsigned_value<std::uint64_t>(root, "stored_size_bytes");
    point.source_count = unsigned_value<std::uint32_t>(root, "source_count");
    point.has_sidecar = root.at("has_sidecar").get<bool>();
    return point;
}

[[nodiscard]] contracts::RecoveryPointPage parse_recovery_point_page(const Json& root) {
    constexpr std::array<std::string_view, 4> keys{"state", "repository_uuid", "items",
                                                   "continuation_token"};
    if (!exact_keys(root, keys) || !root.at("items").is_array()) {
        throw std::invalid_argument("recovery point page fields are invalid");
    }
    contracts::RecoveryPointPage page;
    page.state =
        static_cast<contracts::RepositoryCatalogState>(unsigned_value<std::uint8_t>(root, "state"));
    page.repository_uuid = root.at("repository_uuid").get<std::string>();
    page.continuation_token = optional_string(root.at("continuation_token"));
    for (const auto& item : root.at("items")) {
        page.items.push_back(parse_recovery_point(item));
    }
    return page;
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
    root["recovery_points"] = response.recovery_points
                                  ? encode_recovery_point_page(*response.recovery_points)
                                  : Json(nullptr);
    return root;
}

[[nodiscard]] contracts::ServiceResponse parse_response(const Json& root) {
    constexpr std::array<std::string_view, 7> keys{"schema_version",      "request_id",   "kind",
                                                   "boundary_error_code", "message_code", "service",
                                                   "recovery_points"};
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
    if (!root.at("recovery_points").is_null()) {
        response.recovery_points = parse_recovery_point_page(root.at("recovery_points"));
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
        Json root{
            {"schema_version", request.schema_version},
            {"request_id", request.request_id},
            {"kind", static_cast<std::uint8_t>(request.kind)},
        };
        if (request.repository_list) {
            root["repository_list"] = Json{
                {"maximum_results", request.repository_list->maximum_results},
                {"continuation_token", request.repository_list->continuation_token
                                           ? Json(*request.repository_list->continuation_token)
                                           : Json(nullptr)},
            };
        }
        return base::Result<std::string>::success(root.dump());
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
