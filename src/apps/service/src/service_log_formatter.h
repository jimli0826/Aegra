#pragma once

#include "aegra/contracts/service.h"

#include <string>
#include <string_view>

namespace aegra::apps::service::detail {

[[nodiscard]] std::string_view request_kind_name(contracts::ServiceRequestKind kind) noexcept;
[[nodiscard]] std::string request_log_detail(const contracts::ServiceRequest& request);
[[nodiscard]] std::string response_log_detail(const contracts::ServiceResponse& response);
[[nodiscard]] std::string readable_code(std::string_view code);
[[nodiscard]] std::string sanitized_interaction_detail(std::string_view direction,
                                                       std::string_view encoded);

} // namespace aegra::apps::service::detail
