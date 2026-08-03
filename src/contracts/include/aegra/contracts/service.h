#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kServiceRequestSchemaVersion = 1;
inline constexpr std::uint32_t kServiceResponseSchemaVersion = 1;
inline constexpr std::uint32_t kServiceApiVersion = 1;

enum class ServiceRequestKind : std::uint8_t {
    kGetServiceInfo = 1,
};

enum class ServiceResponseKind : std::uint8_t {
    kServiceInfo = 1,
    kRequestRejected = 2,
};

enum class ServiceState : std::uint8_t {
    kStarting = 1,
    kReady = 2,
    kStopping = 3,
};

struct ServiceRequest final {
    std::uint32_t schema_version{kServiceRequestSchemaVersion};
    std::string request_id;
    ServiceRequestKind kind{ServiceRequestKind::kGetServiceInfo};
};

struct ServiceInfo final {
    std::uint32_t api_version{kServiceApiVersion};
    ServiceState state{ServiceState::kStarting};
    std::string service_version;
    std::vector<std::string> capabilities;
};

struct ServiceResponse final {
    std::uint32_t schema_version{kServiceResponseSchemaVersion};
    std::string request_id;
    ServiceResponseKind kind{ServiceResponseKind::kRequestRejected};
    base::ErrorCode boundary_error_code{base::ErrorCode::kInternal};
    std::string message_code;
    std::optional<ServiceInfo> service;
};

[[nodiscard]] base::Result<void> validate_service_request(const ServiceRequest& request);
[[nodiscard]] base::Result<void> validate_service_info(const ServiceInfo& service);
[[nodiscard]] base::Result<void> validate_service_response(const ServiceResponse& response);

} // namespace aegra::contracts
