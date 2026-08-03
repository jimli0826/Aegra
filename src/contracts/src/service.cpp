#include "aegra/contracts/service.h"

#include <algorithm>
#include <string_view>

namespace aegra::contracts {
namespace {

constexpr std::size_t kMaximumRequestIdBytes = 128;
constexpr std::size_t kMaximumVersionBytes = 64;
constexpr std::size_t kMaximumMessageCodeBytes = 128;
constexpr std::size_t kMaximumCapabilities = 64;
constexpr std::size_t kMaximumCapabilityBytes = 64;

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool valid_code_character(const unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
           value == '_' || value == '-';
}

[[nodiscard]] bool valid_stable_code(const std::string_view value,
                                     const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::all_of(value.begin(), value.end(), [](const unsigned char character) {
               return valid_code_character(character);
           });
}

[[nodiscard]] bool valid_request_id(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumRequestIdBytes) {
        return false;
    }
    return std::all_of(value.begin(), value.end(), [](const unsigned char character) {
        return character >= 0x21U && character <= 0x7EU;
    });
}

[[nodiscard]] base::Result<void> validate_info_response(const ServiceResponse& response) {
    if (!valid_request_id(response.request_id) ||
        response.boundary_error_code != base::ErrorCode::kNone || !response.service) {
        return invalid("service info response fields are invalid");
    }
    return validate_service_info(*response.service);
}

[[nodiscard]] base::Result<void> validate_rejection(const ServiceResponse& response) {
    if (response.service ||
        (response.boundary_error_code != base::ErrorCode::kInvalidArgument &&
         response.boundary_error_code != base::ErrorCode::kUnsupportedVersion)) {
        return invalid("service rejection fields are invalid");
    }
    if (!response.request_id.empty() && !valid_request_id(response.request_id)) {
        return invalid("service rejection request id is invalid");
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_service_request(const ServiceRequest& request) {
    if (request.schema_version != kServiceRequestSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported service request schema version",
        });
    }
    if (!valid_request_id(request.request_id)) {
        return invalid("service request id is invalid");
    }
    if (request.kind != ServiceRequestKind::kGetServiceInfo) {
        return invalid("service request kind is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_info(const ServiceInfo& service) {
    if (service.api_version != kServiceApiVersion || service.state != ServiceState::kReady ||
        service.service_version.empty() || service.service_version.size() > kMaximumVersionBytes ||
        service.capabilities.empty() || service.capabilities.size() > kMaximumCapabilities) {
        return invalid("service info fields are invalid");
    }
    for (const auto& capability : service.capabilities) {
        if (!valid_stable_code(capability, kMaximumCapabilityBytes)) {
            return invalid("service capability is invalid");
        }
    }
    if (!std::is_sorted(service.capabilities.begin(), service.capabilities.end()) ||
        std::adjacent_find(service.capabilities.begin(), service.capabilities.end()) !=
            service.capabilities.end()) {
        return invalid("service capabilities must be sorted and unique");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_response(const ServiceResponse& response) {
    if (response.schema_version != kServiceResponseSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported service response schema version",
        });
    }
    if (!valid_stable_code(response.message_code, kMaximumMessageCodeBytes)) {
        return invalid("service response message code is invalid");
    }
    switch (response.kind) {
    case ServiceResponseKind::kServiceInfo:
        return validate_info_response(response);
    case ServiceResponseKind::kRequestRejected:
        return validate_rejection(response);
    }
    return invalid("service response kind is invalid");
}

} // namespace aegra::contracts
