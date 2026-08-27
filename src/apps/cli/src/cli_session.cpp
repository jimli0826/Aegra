#include "cli_session.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/base/error.h"

#include "cli_ids.h"
#include "cli_io.h"

#include <algorithm>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

namespace aegra::apps::cli {
namespace {

using aegra::adapters::windows_ipc::WindowsNamedPipeChannel;
using aegra::adapters::windows_ipc::WindowsNamedPipeConnectRequest;
using aegra::adapters::windows_ipc::WindowsNamedPipeNamespace;

class BoundedDeadline final {
  public:
    explicit BoundedDeadline(const std::uint32_t timeout_ms) {
        if (timeout_ms == 0) {
            return;
        }
        watcher_ = std::jthread([this, timeout_ms](const std::stop_token stop) {
            const auto deadline =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            while (!stop.stop_requested() && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
            if (!stop.stop_requested()) {
                source_.request_stop();
            }
        });
    }

    BoundedDeadline(const BoundedDeadline&) = delete;
    BoundedDeadline& operator=(const BoundedDeadline&) = delete;

    ~BoundedDeadline() {
        if (watcher_.joinable()) {
            watcher_.request_stop();
        }
    }

    [[nodiscard]] base::CancellationToken token() const noexcept { return source_.get_token(); }

  private:
    base::CancellationSource source_{};
    std::jthread watcher_{};
};

[[nodiscard]] base::Error session_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

} // namespace

ServiceSession::ServiceSession(std::unique_ptr<ports::IMessageChannel> channel,
                               ports::IRandomSource& random, const std::uint32_t timeout_ms)
    : channel_(std::move(channel)), random_(random), timeout_ms_(timeout_ms) {}

base::Result<std::unique_ptr<ServiceSession>>
ServiceSession::connect(const std::uint32_t timeout_ms, ports::IRandomSource& random) {
    WindowsNamedPipeConnectRequest request;
    request.pipe_name = "control";
    request.connect_timeout_ms = (std::min)(timeout_ms, 5'000U);
    request.maximum_frame_bytes = apps::service::kMaximumServiceFrameBytes;
    request.pipe_namespace = WindowsNamedPipeNamespace::kService;
    BoundedDeadline deadline(request.connect_timeout_ms);
    auto channel = WindowsNamedPipeChannel::connect(request, deadline.token());
    if (!channel) {
        return base::Result<std::unique_ptr<ServiceSession>>::failure(
            {channel.error().code, "failed to connect to Aegra Service "
                                   "(\\\\.\\pipe\\aegra-service-control)"});
    }
    std::unique_ptr<ports::IMessageChannel> transport = std::move(channel).value();
    auto session =
        std::make_unique<ServiceSession>(std::move(transport), random, timeout_ms);
    auto ready = session->handshake();
    if (!ready) {
        return base::Result<std::unique_ptr<ServiceSession>>::failure(ready.error());
    }
    return base::Result<std::unique_ptr<ServiceSession>>::success(std::move(session));
}

base::Result<contracts::ServiceResponse> ServiceSession::handshake() {
    auto response = transact(contracts::ServiceRequestKind::kGetServiceInfo,
                             contracts::ServiceVersionRange{contracts::kServiceApiVersion,
                                                            contracts::kServiceApiVersion},
                             false);
    if (!response) {
        return response;
    }
    if (response.value().kind == contracts::ServiceResponseKind::kRequestFailed) {
        write_failed_response(response.value());
        return base::Result<contracts::ServiceResponse>::failure(
            session_error(response.value().boundary_error_code, "service handshake failed"));
    }
    const auto* info = std::get_if<contracts::ServiceInfo>(&response.value().payload);
    if (info == nullptr) {
        return base::Result<contracts::ServiceResponse>::failure(
            session_error(base::ErrorCode::kCorruptData, "service info payload is missing"));
    }
    if (info->api_version != contracts::kServiceApiVersion) {
        return base::Result<contracts::ServiceResponse>::failure(
            session_error(base::ErrorCode::kUnsupportedVersion, "service API version is not 4"));
    }
    if (info->state != contracts::ServiceState::kReady) {
        return base::Result<contracts::ServiceResponse>::failure(
            session_error(base::ErrorCode::kConflict, "service is not ready"));
    }
    info_ = *info;
    return response;
}

base::Result<contracts::ServiceResponse>
ServiceSession::transact(const contracts::ServiceRequestKind kind,
                         contracts::ServiceRequestPayload payload, const bool command) {
    BoundedDeadline deadline(timeout_ms_);
    auto request_id = make_random_uuid(random_, deadline.token());
    if (!request_id) {
        return base::Result<contracts::ServiceResponse>::failure(request_id.error());
    }
    contracts::ServiceRequest request;
    request.request_id = std::move(request_id).value();
    request.kind = kind;
    request.payload = std::move(payload);
    if (command) {
        auto key = make_idempotency_key("cli", random_, deadline.token());
        if (!key) {
            return base::Result<contracts::ServiceResponse>::failure(key.error());
        }
        request.idempotency_key = std::move(key).value();
    }
    auto encoded = apps::service::encode_service_request(request);
    if (!encoded) {
        return base::Result<contracts::ServiceResponse>::failure(encoded.error());
    }
    if (auto sent = channel_->send(encoded.value(), deadline.token()); !sent) {
        return base::Result<contracts::ServiceResponse>::failure(sent.error());
    }
    return receive_response(request.request_id, deadline.token());
}

base::Result<contracts::ServiceResponse>
ServiceSession::receive_response(const std::string_view request_id,
                                 const base::CancellationToken& cancellation) {
    for (int skipped = 0; skipped < 32; ++skipped) {
        auto frame = channel_->receive(cancellation);
        if (!frame) {
            return base::Result<contracts::ServiceResponse>::failure(frame.error());
        }
        auto response = apps::service::decode_service_response(frame.value());
        if (response) {
            if (response.value().request_id != request_id) {
                return base::Result<contracts::ServiceResponse>::failure(session_error(
                    base::ErrorCode::kConflict, "service response request_id mismatch"));
            }
            return response;
        }
        auto event = apps::service::decode_service_event(frame.value());
        if (!event) {
            return base::Result<contracts::ServiceResponse>::failure(response.error());
        }
    }
    return base::Result<contracts::ServiceResponse>::failure(
        session_error(base::ErrorCode::kInternal, "service session received too many events"));
}

base::Result<void> ServiceSession::print_json(const contracts::ServiceResponse& response) {
    auto encoded = apps::service::encode_service_response(response);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    write_line(encoded.value());
    return base::Result<void>::success();
}

} // namespace aegra::apps::cli
