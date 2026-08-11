#include "aegra/apps/service/service_response_fit.h"

#include "service_protocol_json.h"

#include <exception>

namespace aegra::apps::service {
namespace {

using protocol_json::Json;

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

} // namespace

base::Result<std::size_t>
measure_service_response_bytes(const contracts::ServiceResponse& response) {
    auto valid = contracts::validate_service_response(response);
    if (!valid) {
        return base::Result<std::size_t>::failure(valid.error());
    }
    try {
        const auto encoded = encode_response_object(response).dump();
        return base::Result<std::size_t>::success(encoded.size());
    } catch (const std::exception&) {
        return base::Result<std::size_t>::failure(base::Error{
            base::ErrorCode::kInternal,
            "service response measurement encoding failed",
        });
    }
}

} // namespace aegra::apps::service
