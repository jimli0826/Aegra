#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/service.h"

#include <string>
#include <string_view>

namespace aegra::apps::service {

inline constexpr std::size_t kMaximumServiceFrameBytes = 1024U * 1024U;

[[nodiscard]] base::Result<std::string>
encode_service_request(const contracts::ServiceRequest& request);

[[nodiscard]] base::Result<contracts::ServiceRequest>
decode_service_request(std::string_view encoded);

[[nodiscard]] base::Result<std::string>
encode_service_response(const contracts::ServiceResponse& response);

[[nodiscard]] base::Result<contracts::ServiceResponse>
decode_service_response(std::string_view encoded);

[[nodiscard]] base::Result<std::string> encode_service_event(const contracts::ServiceEvent& event);

[[nodiscard]] base::Result<contracts::ServiceEvent> decode_service_event(std::string_view encoded);

} // namespace aegra::apps::service
