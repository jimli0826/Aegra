#include "aegra/apps/worker/worker_session.h"

#include "worker_session_internal.h"

#include "aegra/apps/worker/worker_protocol.h"
#include "aegra/base/error.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/contracts/worker_session.h"

#include <atomic>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

contracts::WorkerResponse boundary_response(const contracts::JobRequest* job,
                                            const contracts::WorkerResponseKind kind,
                                            const base::ErrorCode code, const char* message_code) {
    contracts::WorkerResponse response;
    if (job != nullptr) {
        response.job_id = job->job_id;
        response.trace_id = job->trace_id;
    }
    response.kind = kind;
    response.boundary_error_code = code;
    response.message_code = message_code;
    return response;
}

contracts::WorkerEvent result_event(contracts::WorkerResponse response) {
    contracts::WorkerEvent event;
    event.job_id = response.job_id;
    event.trace_id = response.trace_id;
    event.kind = contracts::WorkerEventKind::kResult;
    event.response = std::move(response);
    return event;
}

base::Result<void> send_event(ports::IMessageChannel& channel, const contracts::WorkerEvent& event,
                              const base::CancellationToken& cancellation) {
    auto encoded = encode_worker_event(event);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    return channel.send(encoded.value(), cancellation);
}

class ChannelProgressSink final : public ports::IProgressSink {
  public:
    ChannelProgressSink(ports::IMessageChannel& channel, base::CancellationSource& cancellation,
                        std::atomic_bool& failed,
                        const base::CancellationToken& transport_cancellation) noexcept
        : channel_(channel), cancellation_(cancellation), failed_(failed),
          transport_cancellation_(transport_cancellation) {}

    void publish(const contracts::TaskProgress& progress) noexcept override {
        try {
            contracts::WorkerEvent event;
            event.job_id = progress.job_id;
            event.trace_id = progress.trace_id;
            event.kind = contracts::WorkerEventKind::kProgress;
            event.progress = progress;
            auto sent = send_event(channel_, event, transport_cancellation_);
            if (!sent) {
                failed_.store(true);
                cancellation_.request_stop();
            }
        } catch (...) {
            failed_.store(true);
            cancellation_.request_stop();
        }
    }

  private:
    ports::IMessageChannel& channel_;
    base::CancellationSource& cancellation_;
    std::atomic_bool& failed_;
    base::CancellationToken transport_cancellation_;
};

struct CommandListenerContext final {
    ports::IMessageChannel& channel;
    const contracts::JobRequest& job;
    base::CancellationSource& cancellation;
    std::atomic_bool& failed;
};

void listen_for_command(CommandListenerContext& context) {
    auto frame = context.channel.receive(context.cancellation.get_token());
    if (!frame) {
        if (!context.cancellation.stop_requested()) {
            context.failed.store(true);
            context.cancellation.request_stop();
        }
        return;
    }
    auto command = decode_worker_command(frame.value());
    if (!command || command.value().job_id != context.job.job_id ||
        command.value().trace_id != context.job.trace_id) {
        context.failed.store(true);
    }
    context.cancellation.request_stop();
}

WorkerHostResult override_failure(const contracts::JobRequest& job, const char* message_code) {
    return {WorkerExitCode::kHostFailure,
            boundary_response(&job, contracts::WorkerResponseKind::kHostFailure,
                              base::ErrorCode::kInternal, message_code)};
}

WorkerExitCode reject_initial_frame(ports::IMessageChannel& channel, const base::ErrorCode code,
                                    const base::CancellationToken& cancellation) {
    auto response = boundary_response(nullptr, contracts::WorkerResponseKind::kRequestRejected,
                                      code, "worker.request_rejected");
    auto sent = send_event(channel, result_event(std::move(response)), cancellation);
    return sent ? WorkerExitCode::kRequestRejected : WorkerExitCode::kHostFailure;
}

class PersonalSessionTaskRunner final : public IWorkerSessionTaskRunner {
  public:
    PersonalSessionTaskRunner(const WindowsPersonalBackupTaskOptions& options,
                              const WindowsPersonalBackupTaskContext& context)
        : options_(options), context_(context) {}

    [[nodiscard]] WorkerHostResult run(const contracts::JobRequest& job,
                                       ports::IProgressSink& progress,
                                       const base::CancellationToken& cancellation) override {
        const WindowsPersonalBackupTaskContext task_context{
            context_.credentials,
            context_.random,
            context_.clock,
            &progress,
        };
        return run_windows_personal_backup_worker_host(job, options_, task_context, cancellation);
    }

  private:
    const WindowsPersonalBackupTaskOptions& options_;
    const WindowsPersonalBackupTaskContext& context_;
};

} // namespace

WorkerExitCode run_worker_session_with_runner(ports::IMessageChannel& channel,
                                              const base::CancellationToken& cancellation,
                                              IWorkerSessionTaskRunner& runner) {
    auto initial_frame = channel.receive(cancellation);
    if (!initial_frame) {
        return WorkerExitCode::kHostFailure;
    }
    auto job = decode_worker_job_request(initial_frame.value());
    if (!job) {
        return reject_initial_frame(channel, job.error().code, cancellation);
    }

    base::CancellationSource session_stop;
    std::stop_callback external_stop(cancellation,
                                     [&session_stop] { session_stop.request_stop(); });
    std::atomic_bool command_failed{false};
    std::atomic_bool progress_failed{false};
    ChannelProgressSink progress(channel, session_stop, progress_failed, cancellation);
    CommandListenerContext listener_context{channel, job.value(), session_stop, command_failed};
    std::jthread listener([&listener_context] { listen_for_command(listener_context); });

    auto result = runner.run(job.value(), progress, session_stop.get_token());
    session_stop.request_stop();
    listener.join();
    if (command_failed.load()) {
        result = override_failure(job.value(), "worker.command_failed");
    } else if (progress_failed.load()) {
        result = override_failure(job.value(), "worker.progress_failed");
    }
    auto sent = send_event(channel, result_event(std::move(result.response)), cancellation);
    return sent ? result.exit_code : WorkerExitCode::kHostFailure;
}

} // namespace detail

WorkerExitCode run_windows_personal_backup_worker_session(
    ports::IMessageChannel& channel, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation) {
    detail::PersonalSessionTaskRunner runner(options, context);
    return detail::run_worker_session_with_runner(channel, cancellation, runner);
}

} // namespace aegra::apps::worker
