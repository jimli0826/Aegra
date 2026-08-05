#include "aegra/apps/service/worker_supervisor.h"

#include "supervisor_worker_protocol.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/random.h"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using adapters::windows_ipc::WindowsNamedPipeListener;

enum class SessionStopReason : std::uint8_t {
    kNone = 0,
    kUser = 1,
    kDeadline = 2,
    kShutdown = 3,
    kTransportFailure = 4,
};

struct SessionDependencies final {
    ports::IProcessLauncher* launcher{nullptr};
    ports::IControlPlaneDatabase* control_plane{nullptr};
    ports::IClock* clock{nullptr};
    SupervisorProgressCallback on_progress;
    SupervisorCompletionCallback on_completion;
    // Shared with Impl for list_jobs progress merge.
    std::mutex* progress_mutex{nullptr};
    std::unordered_map<std::string, contracts::TaskProgress>* last_progress{nullptr};
};

struct WorkerSessionState final {
    std::string job_id;
    std::string trace_id;
    std::uint32_t worker_pid{0};
    std::chrono::steady_clock::time_point deadline;
    base::CancellationSource receive_cancel;
    base::CancellationSource control_io_cancel;
    std::atomic<SessionStopReason> stop_reason{SessionStopReason::kNone};
    std::atomic<std::int64_t> stop_requested_at_ms{0};
    std::atomic<bool> cancel_started{false};
    std::atomic<bool> terminate_started{false};
    std::atomic<bool> completed{false};
};

struct OwnedSession final {
    std::shared_ptr<WorkerSessionState> state;
    std::jthread thread;
};

[[nodiscard]] std::int64_t steady_now_ms() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

[[nodiscard]] std::uint64_t utc_now_ms(const ports::IClock& clock) noexcept {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0U : static_cast<std::uint64_t>(now);
}

void request_session_stop(const std::shared_ptr<WorkerSessionState>& session,
                          const SessionStopReason reason) noexcept {
    auto expected = SessionStopReason::kNone;
    if (session->stop_reason.compare_exchange_strong(expected, reason)) {
        session->stop_requested_at_ms = steady_now_ms();
    }
    session->receive_cancel.request_stop();
}

[[nodiscard]] base::Result<std::string>
generate_pipe_name(ports::IRandomSource& random, const base::CancellationToken cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string result = "aegra-worker-";
    result.reserve(45);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        result.push_back(kHex[value >> 4U]);
        result.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(result));
}

[[nodiscard]] base::Result<contracts::ServiceJobState>
persist_transition(ports::IControlPlaneDatabase& database, ports::IClock& clock,
                   const std::string_view job_id, const contracts::ServiceJobState next_state,
                   std::string message_code, const std::optional<std::uint32_t> result_error,
                   const std::optional<std::uint32_t> result_outcome,
                   std::optional<std::string> result_message) {
    auto current = database.get_job(job_id, {});
    if (!current)
        return base::Result<contracts::ServiceJobState>::failure(current.error());
    if (!current.value()) {
        return base::Result<contracts::ServiceJobState>::failure(
            {base::ErrorCode::kNotFound, "job not found"});
    }
    if (current.value()->state == next_state) {
        return base::Result<contracts::ServiceJobState>::success(next_state);
    }
    auto unit = database.begin_unit_of_work({});
    if (!unit)
        return base::Result<contracts::ServiceJobState>::failure(unit.error());
    ports::JobStateTransition transition;
    transition.job_id = std::string(job_id);
    transition.expected_state = current.value()->state;
    transition.next_state = next_state;
    transition.transition_utc_ms = utc_now_ms(clock);
    transition.message_code = std::move(message_code);
    transition.result_error_code = result_error;
    transition.result_outcome = result_outcome;
    transition.result_message_code = std::move(result_message);
    auto changed = unit.value()->jobs().transition(transition, {});
    if (!changed) {
        unit.value()->rollback();
        return base::Result<contracts::ServiceJobState>::failure(changed.error());
    }
    auto committed = unit.value()->commit({});
    return committed ? base::Result<contracts::ServiceJobState>::success(next_state)
                     : base::Result<contracts::ServiceJobState>::failure(committed.error());
}

[[nodiscard]] contracts::ServiceJobState
terminal_state_for(const contracts::WorkerResponse& response) noexcept {
    if (response.task_result &&
        (response.task_result->outcome == contracts::TaskOutcome::kSucceeded ||
         response.task_result->outcome == contracts::TaskOutcome::kSucceededWithWarning)) {
        return contracts::ServiceJobState::kSucceeded;
    }
    if (response.task_result &&
        response.task_result->outcome == contracts::TaskOutcome::kCancelled) {
        return contracts::ServiceJobState::kCancelled;
    }
    return contracts::ServiceJobState::kFailed;
}

[[nodiscard]] base::Result<contracts::ServiceJobState>
persist_worker_result(const SessionDependencies& dependencies, const std::string_view job_id,
                      const contracts::WorkerResponse& response) {
    const auto state = terminal_state_for(response);
    const auto message = state == contracts::ServiceJobState::kSucceeded   ? "job.succeeded"
                         : state == contracts::ServiceJobState::kCancelled ? "job.cancelled"
                                                                           : "job.failed";
    const auto error = response.task_result
                           ? static_cast<std::uint32_t>(response.task_result->error_code)
                           : static_cast<std::uint32_t>(response.boundary_error_code);
    const auto outcome = response.task_result
                             ? std::optional<std::uint32_t>(
                                   static_cast<std::uint32_t>(response.task_result->outcome))
                             : std::nullopt;
    const auto result_message = response.task_result
                                    ? std::optional<std::string>(response.task_result->message_code)
                                    : std::optional<std::string>(response.message_code);
    return persist_transition(*dependencies.control_plane, *dependencies.clock, job_id, state,
                              message, error, outcome, result_message);
}

[[nodiscard]] base::Result<contracts::ServiceJobState>
persist_missing_result(const SessionDependencies& dependencies,
                       const std::shared_ptr<WorkerSessionState>& session) {
    auto state = contracts::ServiceJobState::kFailed;
    auto error = base::ErrorCode::kInternal;
    std::string message = "service.worker_crashed";
    switch (session->stop_reason.load()) {
    case SessionStopReason::kUser:
        state = contracts::ServiceJobState::kCancelled;
        error = base::ErrorCode::kCancelled;
        message = "job.cancelled";
        break;
    case SessionStopReason::kShutdown:
        state = contracts::ServiceJobState::kInterrupted;
        message = "job.interrupted";
        break;
    case SessionStopReason::kDeadline:
        message = "job.deadline_exceeded";
        break;
    case SessionStopReason::kNone:
    case SessionStopReason::kTransportFailure:
        break;
    }
    return persist_transition(*dependencies.control_plane, *dependencies.clock, session->job_id,
                              state, message, static_cast<std::uint32_t>(error), std::nullopt,
                              message);
}

void publish_completion(const WorkerJobRequest& request,
                        const std::shared_ptr<WorkerSessionState>& session,
                        const std::shared_ptr<SessionDependencies>& dependencies,
                        const base::Result<contracts::ServiceJobState>& persisted,
                        const contracts::WorkerResponse* response) noexcept {
    session->completed = true;
    if (dependencies->progress_mutex != nullptr && dependencies->last_progress != nullptr) {
        std::lock_guard lock(*dependencies->progress_mutex);
        dependencies->last_progress->erase(session->job_id);
    }
    if (!persisted || !dependencies->on_completion)
        return;
    try {
        dependencies->on_completion(request, persisted.value(), response);
    } catch (...) {
        // Observer failures cannot roll back an already persisted terminal state.
    }
}

void terminate_and_wait(const std::shared_ptr<WorkerSessionState>& session,
                        const SessionDependencies& dependencies) noexcept {
    if (session->worker_pid == 0)
        return;
    (void)dependencies.launcher->terminate(session->worker_pid);
    (void)dependencies.launcher->wait(session->worker_pid, {});
}

[[nodiscard]] bool matching_event(const WorkerSessionState& session,
                                  const contracts::WorkerEvent& event) noexcept {
    return event.job_id == session.job_id && event.trace_id == session.trace_id;
}

void send_cancel(const std::shared_ptr<WorkerSessionState>& session,
                 ports::IMessageChannel& channel) noexcept {
    session->cancel_started = true;
    contracts::WorkerCommand command;
    command.job_id = session->job_id;
    command.trace_id = session->trace_id;
    command.kind = contracts::WorkerCommandKind::kCancel;
    auto encoded = encode_supervisor_worker_command(command);
    if (encoded) {
        (void)channel.send(encoded.value(), session->control_io_cancel.get_token());
    }
}

[[nodiscard]] std::optional<contracts::WorkerResponse>
receive_worker_result(const std::shared_ptr<WorkerSessionState>& session,
                      const std::shared_ptr<SessionDependencies>& dependencies,
                      ports::IMessageChannel& channel) {
    for (;;) {
        auto received = channel.receive(session->receive_cancel.get_token());
        if (!received) {
            if (session->stop_reason.load() == SessionStopReason::kNone) {
                request_session_stop(session, SessionStopReason::kTransportFailure);
            }
            return std::nullopt;
        }
        auto decoded = decode_supervisor_worker_event(received.value());
        if (!decoded || !matching_event(*session, decoded.value())) {
            request_session_stop(session, SessionStopReason::kTransportFailure);
            return std::nullopt;
        }
        const auto& event = decoded.value();
        if (event.kind == contracts::WorkerEventKind::kResult && event.response) {
            return *event.response;
        }
        if (event.kind == contracts::WorkerEventKind::kProgress && event.progress) {
            if (dependencies->progress_mutex != nullptr &&
                dependencies->last_progress != nullptr) {
                std::lock_guard lock(*dependencies->progress_mutex);
                (*dependencies->last_progress)[session->job_id] = *event.progress;
            }
            if (dependencies->on_progress) {
                try {
                    dependencies->on_progress(session->job_id, *event.progress);
                } catch (...) {
                    // Progress observers are best-effort and never own Worker session lifetime.
                }
            }
        }
    }
}

void run_session(const std::shared_ptr<WorkerSessionState>& session,
                  const std::shared_ptr<SessionDependencies>& dependencies,
                  std::unique_ptr<WindowsNamedPipeListener> listener,
                  const WorkerJobRequest& request) noexcept {
    try {
        auto channel = listener->accept(session->receive_cancel.get_token());
        if (!channel) {
            terminate_and_wait(session, *dependencies);
            publish_completion(request, session, dependencies,
                               persist_missing_result(*dependencies, session), nullptr);
            return;
        }
        auto encoded = encode_supervisor_job_request(request.worker_request);
        if (!encoded ||
            !channel.value()->send(encoded.value(), session->receive_cancel.get_token())) {
            request_session_stop(session, SessionStopReason::kTransportFailure);
            send_cancel(session, *channel.value());
            (void)dependencies->launcher->wait(session->worker_pid, {});
            publish_completion(request, session, dependencies,
                               persist_missing_result(*dependencies, session), nullptr);
            return;
        }
        auto response = receive_worker_result(session, dependencies, *channel.value());
        if (!response)
            send_cancel(session, *channel.value());
        (void)dependencies->launcher->wait(session->worker_pid, {});
        auto persisted = response ? persist_worker_result(*dependencies, session->job_id, *response)
                                  : persist_missing_result(*dependencies, session);
        publish_completion(request, session, dependencies, persisted,
                           response ? &*response : nullptr);
    } catch (...) {
        request_session_stop(session, SessionStopReason::kTransportFailure);
        terminate_and_wait(session, *dependencies);
        publish_completion(request, session, dependencies,
                           persist_missing_result(*dependencies, session), nullptr);
    }
}

} // namespace

struct WorkerSupervisor::Impl final {
    WorkerSupervisorConfig config;
    ports::IProcessLauncher& launcher;
    ports::IControlPlaneDatabase& control_plane;
    ports::IClock& clock;
    ports::IRandomSource& random;
    std::shared_ptr<SessionDependencies> dependencies;
    std::mutex lifecycle_mutex;
    mutable std::mutex sessions_mutex;
    mutable std::mutex progress_mutex;
    std::unordered_map<std::string, contracts::TaskProgress> last_progress;
    std::unordered_map<std::string, std::unique_ptr<OwnedSession>> sessions;
    std::atomic<bool> shutting_down{false};
    std::jthread monitor_thread;

    Impl(WorkerSupervisorConfig value, ports::IProcessLauncher& process_launcher,
         ports::IControlPlaneDatabase& database, ports::IClock& system_clock,
         ports::IRandomSource& random_source, SupervisorProgressCallback progress,
         SupervisorCompletionCallback completion)
        : config(std::move(value)), launcher(process_launcher), control_plane(database),
          clock(system_clock), random(random_source),
          dependencies(std::make_shared<SessionDependencies>()) {
        dependencies->launcher = &launcher;
        dependencies->control_plane = &control_plane;
        dependencies->clock = &clock;
        dependencies->on_progress = std::move(progress);
        dependencies->on_completion = std::move(completion);
        dependencies->progress_mutex = &progress_mutex;
        dependencies->last_progress = &last_progress;
        monitor_thread = std::jthread([this](const std::stop_token stop) { monitor(stop); });
    }

    void monitor(std::stop_token stop) noexcept;
    void reap_completed();
    [[nodiscard]] base::Result<void>
    reserve_session(const std::shared_ptr<WorkerSessionState>& state);
    void erase_session(std::string_view job_id);
    [[nodiscard]] base::Result<void> launch_worker(const WorkerJobRequest& request,
                                                   const contracts::JobRequest& worker_request,
                                                   std::string_view pipe_name,
                                                   const std::shared_ptr<WorkerSessionState>& state,
                                                   base::CancellationToken cancellation);
    [[nodiscard]] base::Result<void>
    start_session_thread(const std::shared_ptr<WorkerSessionState>& state,
                          std::unique_ptr<WindowsNamedPipeListener> listener,
                          WorkerJobRequest request) noexcept;
};

void WorkerSupervisor::Impl::reap_completed() {
    std::vector<std::unique_ptr<OwnedSession>> completed;
    {
        std::lock_guard lock(sessions_mutex);
        for (auto it = sessions.begin(); it != sessions.end();) {
            if (!it->second->state->completed) {
                ++it;
                continue;
            }
            completed.push_back(std::move(it->second));
            it = sessions.erase(it);
        }
    }
}

void WorkerSupervisor::Impl::monitor(const std::stop_token stop) noexcept {
    while (!stop.stop_requested()) {
        std::vector<std::shared_ptr<WorkerSessionState>> snapshot;
        {
            std::lock_guard lock(sessions_mutex);
            for (const auto& [job_id, owner] : sessions)
                snapshot.push_back(owner->state);
        }
        const auto now = std::chrono::steady_clock::now();
        const auto now_ms = steady_now_ms();
        for (const auto& session : snapshot) {
            if (session->completed)
                continue;
            if (session->stop_reason.load() == SessionStopReason::kNone &&
                now >= session->deadline) {
                request_session_stop(session, SessionStopReason::kDeadline);
            }
            const auto requested_at = session->stop_requested_at_ms.load();
            const auto grace_ms =
                std::chrono::duration_cast<std::chrono::milliseconds>(config.stop_drain_timeout)
                    .count();
            if (session->cancel_started && requested_at > 0 && now_ms - requested_at >= grace_ms &&
                session->worker_pid != 0 && !session->terminate_started.exchange(true)) {
                session->control_io_cancel.request_stop();
                (void)launcher.terminate(session->worker_pid);
            }
        }
        reap_completed();
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
}

base::Result<void>
WorkerSupervisor::Impl::reserve_session(const std::shared_ptr<WorkerSessionState>& state) {
    std::lock_guard lock(sessions_mutex);
    if (sessions.size() >= config.max_concurrent_workers || sessions.contains(state->job_id)) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "worker capacity or job id conflict"});
    }
    auto owner = std::make_unique<OwnedSession>();
    owner->state = state;
    sessions.emplace(state->job_id, std::move(owner));
    return base::Result<void>::success();
}

void WorkerSupervisor::Impl::erase_session(const std::string_view job_id) {
    std::lock_guard lock(sessions_mutex);
    sessions.erase(std::string(job_id));
}

base::Result<void> WorkerSupervisor::Impl::launch_worker(
    const WorkerJobRequest& request, const contracts::JobRequest& worker_request,
    const std::string_view pipe_name, const std::shared_ptr<WorkerSessionState>& state,
    const base::CancellationToken cancellation) {
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit)
        return base::Result<void>::failure(unit.error());
    ports::JobRecord record;
    record.job_id = state->job_id;
    record.trace_id = state->trace_id;
    record.operation = worker_request.operation;
    record.state = contracts::ServiceJobState::kQueued;
    record.created_utc_ms = utc_now_ms(clock);
    record.source_ids = request.source_ids;
    record.repository_connection_id = request.repository_connection_id;
    record.backup_type =
        worker_request.backup ? std::optional(worker_request.backup->type) : std::nullopt;
    record.parent_recovery_point_id = request.parent_recovery_point_id;
    if (worker_request.backup) {
        record.exclude_page_and_hibernation_files =
            worker_request.backup->exclude_page_and_hibernation_files;
    }
    record.message_code = "job.queued";
    record.idempotency_key = request.idempotency_key;
    auto inserted = unit.value()->jobs().insert(record, cancellation);
    if (!inserted) {
        unit.value()->rollback();
        return base::Result<void>::failure(inserted.error());
    }
    auto queued_committed = unit.value()->commit(cancellation);
    if (!queued_committed)
        return queued_committed;

    auto launched =
        launcher.launch({config.worker_executable_path, {"--pipe", std::string(pipe_name)}});
    if (!launched) {
        auto failed = persist_transition(
            control_plane, clock, state->job_id, contracts::ServiceJobState::kFailed,
            "service.worker_launch_failed", static_cast<std::uint32_t>(launched.error().code),
            std::nullopt, "service.worker_launch_failed");
        return failed ? base::Result<void>::failure(launched.error())
                      : base::Result<void>::failure(failed.error());
    }
    state->worker_pid = launched.value().pid;
    auto running = persist_transition(control_plane, clock, state->job_id,
                                      contracts::ServiceJobState::kRunning, "job.running",
                                      std::nullopt, std::nullopt, std::nullopt);
    if (running)
        return base::Result<void>::success();

    terminate_and_wait(state, *dependencies);
    (void)persist_transition(
        control_plane, clock, state->job_id, contracts::ServiceJobState::kFailed,
        "service.job_start_persistence_failed", static_cast<std::uint32_t>(running.error().code),
        std::nullopt, "service.job_start_persistence_failed");
    return base::Result<void>::failure(running.error());
}

base::Result<void>
WorkerSupervisor::Impl::start_session_thread(const std::shared_ptr<WorkerSessionState>& state,
                                              std::unique_ptr<WindowsNamedPipeListener> listener,
                                              WorkerJobRequest request) noexcept {
    try {
        std::lock_guard lock(sessions_mutex);
        auto& owner = sessions.at(state->job_id);
        owner->thread =
            std::jthread([state, dependencies = dependencies, listener = std::move(listener),
                          request = std::move(request)](std::stop_token) mutable {
                run_session(state, dependencies, std::move(listener), request);
            });
        return base::Result<void>::success();
    } catch (...) {
        request_session_stop(state, SessionStopReason::kTransportFailure);
        terminate_and_wait(state, *dependencies);
        (void)persist_missing_result(*dependencies, state);
        state->completed = true;
        return base::Result<void>::failure(
            {base::ErrorCode::kInternal, "failed to start Worker session thread"});
    }
}

WorkerSupervisor::WorkerSupervisor(WorkerSupervisorConfig config, ports::IProcessLauncher& launcher,
                                   ports::IControlPlaneDatabase& control_plane,
                                   ports::IClock& clock, ports::IRandomSource& random,
                                   SupervisorProgressCallback on_progress,
                                   SupervisorCompletionCallback on_completion)
    : impl_(std::make_unique<Impl>(std::move(config), launcher, control_plane, clock, random,
                                   std::move(on_progress), std::move(on_completion))) {}

WorkerSupervisor::~WorkerSupervisor() { shutdown({}); }

base::Result<void> WorkerSupervisor::submit(const WorkerJobRequest& request,
                                            const base::CancellationToken cancel) {
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
    if (impl_->shutting_down || impl_->config.worker_executable_path.empty() ||
        impl_->config.max_concurrent_workers == 0 || request.source_ids.empty() ||
        request.repository_connection_id.empty() || request.idempotency_key.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "worker submission is invalid"});
    }
    impl_->reap_completed();
    const auto deadline =
        request.deadline.count() > 0 ? request.deadline : impl_->config.default_job_deadline;
    auto worker_request = request.worker_request;
    worker_request.deadline_utc_ms =
        impl_->clock.now_utc_ms() +
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline).count();
    if (auto valid = contracts::validate_job_request(worker_request); !valid) {
        return base::Result<void>::failure(valid.error());
    }
    auto pipe_name = generate_pipe_name(impl_->random, cancel);
    if (!pipe_name)
        return base::Result<void>::failure(pipe_name.error());
    auto listener = WindowsNamedPipeListener::create(
        {pipe_name.value(), 1024U * 1024U,
         adapters::windows_ipc::WindowsNamedPipeAclProfile::kProcessDefault,
         adapters::windows_ipc::WindowsNamedPipeNamespace::kWorker});
    if (!listener)
        return base::Result<void>::failure(listener.error());

    auto state = std::make_shared<WorkerSessionState>();
    state->job_id = worker_request.job_id;
    state->trace_id = worker_request.trace_id;
    state->deadline = std::chrono::steady_clock::now() + deadline;
    auto reserved = impl_->reserve_session(state);
    if (!reserved)
        return reserved;
    auto launched = impl_->launch_worker(request, worker_request, pipe_name.value(), state, cancel);
    if (!launched) {
        impl_->erase_session(state->job_id);
        return launched;
    }
    auto session_request = request;
    session_request.worker_request = std::move(worker_request);
    auto started = impl_->start_session_thread(state, std::move(listener).value(),
                                               std::move(session_request));
    if (!started)
        impl_->erase_session(state->job_id);
    return started;
}

base::Result<void> WorkerSupervisor::cancel_job(const std::string_view job_id,
                                                const base::CancellationToken cancel) {
    if (cancel.stop_requested()) {
        return base::Result<void>::failure({base::ErrorCode::kCancelled, "job cancel requested"});
    }
    std::shared_ptr<WorkerSessionState> session;
    {
        std::lock_guard lock(impl_->sessions_mutex);
        const auto found = impl_->sessions.find(std::string(job_id));
        if (found == impl_->sessions.end() || found->second->state->completed) {
            return base::Result<void>::failure({base::ErrorCode::kNotFound, "job not active"});
        }
        session = found->second->state;
    }
    request_session_stop(session, SessionStopReason::kUser);
    return base::Result<void>::success();
}

void WorkerSupervisor::shutdown(const base::CancellationToken& cancel) {
    (void)cancel;
    std::unique_lock lifecycle_lock(impl_->lifecycle_mutex);
    if (impl_->shutting_down.exchange(true))
        return;
    std::vector<std::shared_ptr<WorkerSessionState>> states;
    {
        std::lock_guard lock(impl_->sessions_mutex);
        for (const auto& [job_id, owner] : impl_->sessions)
            states.push_back(owner->state);
    }
    for (const auto& state : states)
        request_session_stop(state, SessionStopReason::kShutdown);
    const auto deadline = std::chrono::steady_clock::now() + impl_->config.stop_drain_timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        impl_->reap_completed();
        if (active_count() == 0)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    std::vector<std::unique_ptr<OwnedSession>> owners;
    {
        std::lock_guard lock(impl_->sessions_mutex);
        for (auto& [job_id, owner] : impl_->sessions) {
            owner->state->control_io_cancel.request_stop();
            if (owner->state->worker_pid != 0) {
                (void)impl_->launcher.terminate(owner->state->worker_pid);
            }
            owners.push_back(std::move(owner));
        }
        impl_->sessions.clear();
    }
    impl_->monitor_thread.request_stop();
    if (impl_->monitor_thread.joinable())
        impl_->monitor_thread.join();
}

std::uint32_t WorkerSupervisor::active_count() const noexcept {
    std::lock_guard lock(impl_->sessions_mutex);
    std::uint32_t count = 0;
    for (const auto& [job_id, owner] : impl_->sessions) {
        if (!owner->state->completed)
            ++count;
    }
    return count;
}

std::optional<contracts::TaskProgress>
WorkerSupervisor::last_progress(const std::string_view job_id) const {
    std::lock_guard lock(impl_->progress_mutex);
    const auto found = impl_->last_progress.find(std::string(job_id));
    if (found == impl_->last_progress.end()) {
        return std::nullopt;
    }
    return found->second;
}

} // namespace aegra::apps::service
