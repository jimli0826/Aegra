#pragma once

#include "aegra/contracts/service.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service::protocol_json {

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

[[nodiscard]] std::optional<std::string> optional_string(const Json& value);
[[nodiscard]] Json optional_string_json(const std::optional<std::string>& value);
[[nodiscard]] std::optional<std::uint64_t> optional_uint64(const Json& value);
[[nodiscard]] Json optional_uint64_json(const std::optional<std::uint64_t>& value);
[[nodiscard]] Json encode_message_arguments(const contracts::MessageArguments& arguments);
[[nodiscard]] contracts::MessageArguments parse_message_arguments(const Json& value);

[[nodiscard]] Json encode_request_payload(const contracts::ServiceRequest& request);
[[nodiscard]] contracts::ServiceRequestPayload
parse_request_payload(contracts::ServiceRequestKind kind, const Json& payload);

[[nodiscard]] Json encode_response_payload(const contracts::ServiceResponse& response);
[[nodiscard]] contracts::ServiceResponsePayload
parse_response_payload(contracts::ServiceResponseKind response_kind,
                       contracts::ServiceRequestKind request_kind, const Json& payload);

[[nodiscard]] Json encode_event_payload(const contracts::ServiceEvent& event);
[[nodiscard]] contracts::ServiceEventPayload parse_event_payload(contracts::ServiceEventKind kind,
                                                                 const Json& payload);

} // namespace aegra::apps::service::protocol_json
