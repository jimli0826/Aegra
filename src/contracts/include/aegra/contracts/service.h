#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/repository_query.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kServiceRequestSchemaVersion = 2;
inline constexpr std::uint32_t kServiceResponseSchemaVersion = 2;
inline constexpr std::uint32_t kServiceApiVersion = 2;

enum class ServiceRequestKind : std::uint8_t {
    kGetServiceInfo = 1,
    kListRecoveryPoints = 2,
};

enum class ServiceResponseKind : std::uint8_t {
    kServiceInfo = 1,
    kRequestFailed = 2,
    kRecoveryPointPage = 3,
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
    std::optional<RecoveryPointListRequest> repository_list;
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
    ServiceResponseKind kind{ServiceResponseKind::kRequestFailed};
    base::ErrorCode boundary_error_code{base::ErrorCode::kInternal};
    std::string message_code;
    std::optional<ServiceInfo> service;
    std::optional<RecoveryPointPage> recovery_points;
};

[[nodiscard]] base::Result<void> validate_service_request(const ServiceRequest& request);
[[nodiscard]] base::Result<void> validate_service_info(const ServiceInfo& service);
[[nodiscard]] base::Result<void> validate_service_response(const ServiceResponse& response);

} // namespace aegra::contracts
