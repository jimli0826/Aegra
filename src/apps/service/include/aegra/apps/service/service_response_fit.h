#pragma once

#include "aegra/apps/service/service_protocol.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service.h"
#include "aegra/contracts/service_control.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>

namespace aegra::apps::service {

/// Hard wire limit for one Named Pipe service frame (request or response body).
inline constexpr std::size_t kServiceResponseFitBudgetBytes = kMaximumServiceFrameBytes;

/// Measure encoded response size without enforcing the frame budget (for page packing).
[[nodiscard]] base::Result<std::size_t>
measure_service_response_bytes(const contracts::ServiceResponse& response);

/// Fetch a list payload that fits in one service frame.
/// `fetch(max)` must honor maximum_results and produce a correct continuation_token for the
/// returned prefix. `item_count` reports how many array elements the payload carries.
/// When even a single item cannot fit, returns kInvalidArgument.
template <typename Payload>
[[nodiscard]] base::Result<Payload> fetch_payload_within_frame_budget(
    contracts::ServiceResponse response_shell, const std::uint32_t requested_maximum_results,
    const std::function<base::Result<Payload>(std::uint32_t maximum_results)>& fetch,
    const std::function<std::size_t(const Payload&)>& item_count) {
    std::uint32_t maximum = requested_maximum_results == 0
                                ? contracts::kMaximumServicePageResults
                                : requested_maximum_results;
    maximum = (std::min)(maximum, contracts::kMaximumServicePageResults);

    for (int attempt = 0; attempt < 32; ++attempt) {
        auto payload = fetch(maximum);
        if (!payload) {
            return payload;
        }
        response_shell.payload = payload.value();
        auto measured = measure_service_response_bytes(response_shell);
        if (!measured) {
            return base::Result<Payload>::failure(measured.error());
        }
        if (measured.value() <= kServiceResponseFitBudgetBytes) {
            return payload;
        }
        const auto count = item_count(payload.value());
        if (count <= 1) {
            return base::Result<Payload>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "list page exceeds service frame budget even with a single item",
            });
        }
        auto next_maximum = static_cast<std::uint32_t>(
            (std::max)(std::size_t{1},
                       (count * kServiceResponseFitBudgetBytes) / measured.value()));
        if (next_maximum >= count) {
            next_maximum = static_cast<std::uint32_t>(count - 1U);
        }
        maximum = next_maximum;
    }
    return base::Result<Payload>::failure(base::Error{
        base::ErrorCode::kInternal,
        "list page frame packing did not converge",
    });
}

/// Convenience for `ServicePage<T>` and other payloads with a top-level `items` vector.
template <typename Page>
[[nodiscard]] base::Result<Page> fetch_page_within_frame_budget(
    contracts::ServiceResponse response_shell, const std::uint32_t requested_maximum_results,
    const std::function<base::Result<Page>(std::uint32_t maximum_results)>& fetch) {
    return fetch_payload_within_frame_budget<Page>(
        std::move(response_shell), requested_maximum_results, fetch,
        [](const Page& page) { return page.items.size(); });
}

} // namespace aegra::apps::service
