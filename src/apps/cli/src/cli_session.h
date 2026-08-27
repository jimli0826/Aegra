#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service.h"
#include "aegra/ports/message_channel.h"
#include "aegra/ports/random.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace aegra::apps::cli {

class ServiceSession final {
  public:
    ServiceSession(const ServiceSession&) = delete;
    ServiceSession& operator=(const ServiceSession&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<ServiceSession>>
    connect(std::uint32_t timeout_ms, ports::IRandomSource& random);

    [[nodiscard]] const contracts::ServiceInfo& info() const noexcept { return info_; }

    [[nodiscard]] base::Result<contracts::ServiceResponse>
    transact(contracts::ServiceRequestKind kind, contracts::ServiceRequestPayload payload,
             bool command);

    [[nodiscard]] base::Result<void> print_json(const contracts::ServiceResponse& response);

    ServiceSession(std::unique_ptr<ports::IMessageChannel> channel, ports::IRandomSource& random,
                   std::uint32_t timeout_ms);

  private:
    [[nodiscard]] base::Result<contracts::ServiceResponse> handshake();
    [[nodiscard]] base::Result<contracts::ServiceResponse>
    receive_response(std::string_view request_id, const base::CancellationToken& cancellation);

    std::unique_ptr<ports::IMessageChannel> channel_;
    ports::IRandomSource& random_;
    std::uint32_t timeout_ms_{30'000};
    contracts::ServiceInfo info_{};
};

} // namespace aegra::apps::cli
