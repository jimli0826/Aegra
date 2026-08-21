#include "service_session_executor.h"

#include "aegra/application/repository_connection_service.h"
#include "aegra/apps/service/service_protocol.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>
#include <utility>
#include <variant>
#include <vector>

namespace aegra::apps::service::detail {
namespace {

using Clock = std::chrono::steady_clock;
constexpr auto kRequestDeadline = std::chrono::seconds(30);
constexpr std::size_t kLaneCount = 4;
constexpr std::size_t kMaximumQueuedPerLane = 32;

template <typename Value> class BlockingQueue final {
  public:
    explicit BlockingQueue(const std::size_t capacity) : capacity_(capacity) {}

    [[nodiscard]] bool push(Value value) {
        std::lock_guard lock(mutex_);
        if (closed_ || values_.size() >= capacity_) {
            return false;
        }
        values_.push_back(std::move(value));
        ready_.notify_one();
        return true;
    }

    [[nodiscard]] std::optional<Value> pop(const std::stop_token cancellation) {
        std::unique_lock lock(mutex_);
        std::stop_callback wake(cancellation, [this] { ready_.notify_all(); });
        ready_.wait(lock,
                    [&] { return closed_ || !values_.empty() || cancellation.stop_requested(); });
        if (values_.empty()) {
            return std::nullopt;
        }
        Value value = std::move(values_.front());
        values_.pop_front();
        return value;
    }

    void close() {
        std::lock_guard lock(mutex_);
        closed_ = true;
        ready_.notify_all();
    }

  private:
    const std::size_t capacity_;
    std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Value> values_;
    bool closed_{false};
};

struct RequestState final {
    contracts::ServiceRequest request;
    std::string encoded;
    // Deadline starts when a worker begins execution, not when the request is
    // queued. Otherwise a hung network TestRepositoryConnection burns the
    // deadline of every following command in the same lane and mark them
    // Unavailable without ever opening their locator (false Offline for local).
    Clock::time_point deadline{Clock::time_point::max()};
    base::CancellationSource cancellation;
    std::atomic_bool response_emitted{false};
};

[[nodiscard]] std::size_t lane_for(const contracts::ServiceRequestKind kind) noexcept {
    using Kind = contracts::ServiceRequestKind;
    switch (kind) {
    case Kind::kListRecoveryPoints:
    case Kind::kResolveRecoveryPointChain:
    case Kind::kPlanDeleteRecoveryPoints:
    case Kind::kGetRecoveryPointLayout:
    case Kind::kListRecoveryPointEntries:
    case Kind::kPrepareRestore:
    case Kind::kAnalyzeNtfsShrink:
    case Kind::kPrepareFileRestore:
        return 1;
    case Kind::kBrowseFileSources:
    case Kind::kListRepositoryDirectories:
        return 2;
    case Kind::kAddRepositoryConnection:
    case Kind::kImportRepositoryConnection:
    case Kind::kConnectRepositoryLocation:
    case Kind::kTestRepositoryConnection:
    case Kind::kSetDefaultRepository:
    case Kind::kRemoveRepositoryConnection:
    case Kind::kStartBackup:
    case Kind::kCancelJob:
    case Kind::kStartVerify:
    case Kind::kStartRestore:
    case Kind::kMountRecoveryPoint:
    case Kind::kUnmountSession:
    case Kind::kUpsertSchedule:
    case Kind::kDeleteSchedule:
    case Kind::kSubscribeTaskEvents:
    case Kind::kAcknowledgeEvents:
    case Kind::kExecuteDeletePlan:
    case Kind::kStartFileRestore:
    case Kind::kUpdateServiceSettings:
        return 3;
    default:
        return 0;
    }
}

[[nodiscard]] std::string failure_response(const contracts::ServiceRequest& request,
                                           const base::ErrorCode code, std::string message_code) {
    contracts::ServiceResponse response;
    response.request_id = request.request_id;
    response.kind = contracts::ServiceResponseKind::kRequestFailed;
    response.request_kind = request.kind;
    response.boundary_error_code = code;
    response.message_code = std::move(message_code);
    auto encoded = encode_service_response(response);
    return encoded ? std::move(encoded).value() : std::string{};
}

class SessionExecutor final {
  public:
    SessionExecutor(ports::IMessageChannel& channel, const ServiceRuntimeInfo& runtime,
                    const ServiceSessionContext& session,
                    const base::CancellationToken cancellation)
        : channel_(channel), runtime_(runtime), session_(session), responses_(128) {
        session_stop_callback_.emplace(cancellation, [this] { session_stop_.request_stop(); });
        for (auto& lane : lanes_) {
            lane = std::make_unique<BlockingQueue<std::shared_ptr<RequestState>>>(
                kMaximumQueuedPerLane);
        }
    }

    [[nodiscard]] base::Result<void> run(const std::size_t maximum_requests) {
        start_threads();
        auto receive_result = receive_requests(maximum_requests);
        stop_and_join();
        if (!receive_result) {
            return receive_result;
        }
        std::lock_guard lock(error_mutex_);
        return writer_error_ ? base::Result<void>::failure(*writer_error_)
                             : base::Result<void>::success();
    }

  private:
    void start_threads() {
        writer_ = std::jthread([this](const std::stop_token token) { write_responses(token); });
        deadline_monitor_ =
            std::jthread([this](const std::stop_token token) { monitor_deadlines(token); });
        for (std::size_t index = 0; index < kLaneCount; ++index) {
            workers_[index] = std::jthread(
                [this, index](const std::stop_token token) { execute_lane(index, token); });
        }
    }

    [[nodiscard]] base::Result<void> receive_requests(const std::size_t maximum_requests) {
        std::size_t received = 0;
        while (!session_stop_.stop_requested() &&
               (maximum_requests == 0 || received < maximum_requests)) {
            auto encoded = channel_.receive(session_stop_.get_token());
            if (!encoded) {
                session_stop_.request_stop();
                return base::Result<void>::failure(encoded.error());
            }
            ++received;
            auto decoded = decode_service_request(encoded.value());
            if (!decoded) {
                auto response = handle_service_message(encoded.value(), runtime_, session_, {});
                if (!response) {
                    session_stop_.request_stop();
                    return base::Result<void>::failure(response.error());
                }
                if (!responses_.push(std::move(response).value())) {
                    return queue_failure();
                }
                continue;
            }
            auto state = std::make_shared<RequestState>();
            state->request = std::move(decoded).value();
            state->encoded = std::move(encoded).value();
            {
                std::lock_guard lock(states_mutex_);
                states_.push_back(state);
            }
            auto& lane = *lanes_[lane_for(state->request.kind)];
            if (!lane.push(state)) {
                emit_once(state, failure_response(state->request, base::ErrorCode::kConflict,
                                                  "service.request_queue_full"));
            }
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> queue_failure() {
        session_stop_.request_stop();
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInternal, "service response queue is unavailable"});
    }

    void execute_lane(const std::size_t index, const std::stop_token worker_stop) {
        while (!worker_stop.stop_requested()) {
            auto state = lanes_[index]->pop(worker_stop);
            if (!state) {
                return;
            }
            std::stop_callback session_cancel(session_stop_.get_token(),
                                              [&state] { (*state)->cancellation.request_stop(); });
            if ((*state)->response_emitted.load()) {
                continue;
            }
            {
                std::lock_guard lock(states_mutex_);
                (*state)->deadline = Clock::now() + kRequestDeadline;
            }
            auto response = handle_service_message((*state)->encoded, runtime_, session_,
                                                   (*state)->cancellation.get_token());
            if (!response) {
                emit_once(*state, failure_response((*state)->request, response.error().code,
                                                   "service.request_failed"));
                continue;
            }
            emit_once(*state, std::move(response).value());
        }
    }

    void monitor_deadlines(const std::stop_token monitor_stop) {
        while (!monitor_stop.stop_requested() && !session_stop_.stop_requested()) {
            const auto now = Clock::now();
            std::vector<std::shared_ptr<RequestState>> expired;
            {
                std::lock_guard lock(states_mutex_);
                for (const auto& state : states_) {
                    if (now >= state->deadline && claim_response(state)) {
                        state->cancellation.request_stop();
                        expired.push_back(state);
                    }
                }
                std::erase_if(states_,
                              [](const auto& state) { return state->response_emitted.load(); });
            }
            for (const auto& state : expired) {
                persist_probe_timeout(*state);
                enqueue_claimed_response(failure_response(
                    state->request, base::ErrorCode::kCancelled, "service.request_timeout"));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    [[nodiscard]] static bool claim_response(const std::shared_ptr<RequestState>& state) {
        bool expected = false;
        return state->response_emitted.compare_exchange_strong(expected, true);
    }

    void persist_probe_timeout(const RequestState& state) {
        using Kind = contracts::ServiceRequestKind;
        if (state.request.kind != Kind::kTestRepositoryConnection ||
            runtime_.repository_connections == nullptr) {
            return;
        }
        const auto* reference = std::get_if<contracts::ResourceRef>(&state.request.payload);
        if (reference == nullptr) {
            return;
        }
        const auto persisted = runtime_.repository_connections->mark_connection_unavailable(
            *reference, base::CancellationToken{});
        if (!persisted && runtime_.logger != nullptr) {
            runtime_.logger->write(ServiceLogLevel::kWarning,
                                   "service.repository_probe_timeout_persist_failed",
                                   "request_id=" + state.request.request_id);
        }
    }

    void enqueue_claimed_response(std::string response) {
        if (response.empty() || !responses_.push(std::move(response))) {
            session_stop_.request_stop();
        }
    }

    void emit_once(const std::shared_ptr<RequestState>& state, std::string response) {
        if (!claim_response(state)) {
            return;
        }
        enqueue_claimed_response(std::move(response));
    }

    void write_responses(const std::stop_token writer_stop) {
        while (!writer_stop.stop_requested()) {
            auto response = responses_.pop(writer_stop);
            if (!response) {
                return;
            }
            auto sent = channel_.send(*response, session_stop_.get_token());
            if (!sent) {
                {
                    std::lock_guard lock(error_mutex_);
                    writer_error_ = sent.error();
                }
                session_stop_.request_stop();
                return;
            }
        }
    }

    void stop_and_join() {
        for (auto& lane : lanes_) {
            lane->close();
        }
        for (auto& worker : workers_) {
            worker.join();
        }
        deadline_monitor_.request_stop();
        deadline_monitor_.join();
        responses_.close();
        writer_.join();
    }

    ports::IMessageChannel& channel_;
    const ServiceRuntimeInfo& runtime_;
    const ServiceSessionContext& session_;
    base::CancellationSource session_stop_;
    std::optional<std::stop_callback<std::function<void()>>> session_stop_callback_;
    std::array<std::unique_ptr<BlockingQueue<std::shared_ptr<RequestState>>>, kLaneCount> lanes_;
    BlockingQueue<std::string> responses_;
    std::array<std::jthread, kLaneCount> workers_;
    std::jthread deadline_monitor_;
    std::jthread writer_;
    std::mutex states_mutex_;
    std::vector<std::shared_ptr<RequestState>> states_;
    std::mutex error_mutex_;
    std::optional<base::Error> writer_error_;
};

} // namespace

base::Result<void> run_concurrent_service_session(ports::IMessageChannel& channel,
                                                  const ServiceRuntimeInfo& runtime,
                                                  const ServiceSessionContext& session,
                                                  const base::CancellationToken& cancellation,
                                                  const std::size_t maximum_requests) {
    SessionExecutor executor(channel, runtime, session, cancellation);
    return executor.run(maximum_requests);
}

} // namespace aegra::apps::service::detail
