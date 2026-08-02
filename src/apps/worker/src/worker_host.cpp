#include "aegra/apps/worker/worker_host.h"

#include "worker_host_internal.h"

#include "aegra/apps/worker/personal_archive_verify_task.h"
#include "aegra/base/error.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace aegra::apps::worker {
namespace detail {
namespace {

class DeadlineCancellation final {
  public:
    DeadlineCancellation(const std::int64_t deadline_utc_ms, const std::int64_t now_utc_ms,
                         const base::CancellationToken& external)
        : external_callback_(external, ExternalCancellation{&cancellation_}) {
        if (external.stop_requested()) {
            cancellation_.request_stop();
        }
        start_deadline(deadline_utc_ms, now_utc_ms);
    }

    ~DeadlineCancellation() {
        {
            std::lock_guard lock(mutex_);
            completed_ = true;
        }
        changed_.notify_all();
    }

    DeadlineCancellation(const DeadlineCancellation&) = delete;
    DeadlineCancellation& operator=(const DeadlineCancellation&) = delete;
    DeadlineCancellation(DeadlineCancellation&&) = delete;
    DeadlineCancellation& operator=(DeadlineCancellation&&) = delete;

    [[nodiscard]] base::CancellationToken token() const noexcept {
        return cancellation_.get_token();
    }

  private:
    struct ExternalCancellation final {
        base::CancellationSource* cancellation;

        void operator()() const noexcept { cancellation->request_stop(); }
    };

    void start_deadline(const std::int64_t deadline_utc_ms, const std::int64_t now_utc_ms) {
        if (deadline_utc_ms == 0) {
            return;
        }
        if (deadline_utc_ms <= now_utc_ms) {
            cancellation_.request_stop();
            return;
        }
        const auto remaining = std::chrono::milliseconds(deadline_utc_ms - now_utc_ms);
        watchdog_ = std::jthread([this, remaining] { wait_for_deadline(remaining); });
    }

    void wait_for_deadline(const std::chrono::milliseconds remaining) {
        std::unique_lock lock(mutex_);
        if (!changed_.wait_for(lock, remaining, [this] { return completed_; })) {
            cancellation_.request_stop();
        }
    }

    base::CancellationSource cancellation_;
    std::stop_callback<ExternalCancellation> external_callback_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool completed_{false};
    std::jthread watchdog_;
};

WorkerExitCode exit_code_for(const contracts::TaskOutcome outcome) noexcept {
    switch (outcome) {
    case contracts::TaskOutcome::kSucceeded:
    case contracts::TaskOutcome::kSucceededWithWarning:
        return WorkerExitCode::kSucceeded;
    case contracts::TaskOutcome::kFailed:
        return WorkerExitCode::kTaskFailed;
    case contracts::TaskOutcome::kCancelled:
        return WorkerExitCode::kCancelled;
    }
    return WorkerExitCode::kHostFailure;
}

contracts::WorkerResponse task_response(contracts::TaskResult task_result) {
    contracts::WorkerResponse response;
    response.job_id = task_result.job_id;
    response.trace_id = task_result.trace_id;
    response.kind = contracts::WorkerResponseKind::kTaskResult;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "worker.task_finished";
    response.task_result = std::move(task_result);
    return response;
}

contracts::WorkerResponse boundary_response(const contracts::JobRequest& job,
                                            const contracts::WorkerResponseKind kind,
                                            const base::ErrorCode code, const char* message_code) {
    contracts::WorkerResponse response;
    response.job_id = job.job_id;
    response.trace_id = job.trace_id;
    response.kind = kind;
    response.boundary_error_code = code;
    response.message_code = message_code;
    return response;
}

WorkerHostResult from_execution(const contracts::JobRequest& job,
                                base::Result<contracts::TaskResult> execution) {
    if (execution) {
        auto task_result = std::move(execution).value();
        const auto exit_code = exit_code_for(task_result.outcome);
        return {exit_code, task_response(std::move(task_result))};
    }
    const auto code = execution.error().code;
    if (code == base::ErrorCode::kInvalidArgument || code == base::ErrorCode::kUnsupportedVersion) {
        return {WorkerExitCode::kRequestRejected,
                boundary_response(job, contracts::WorkerResponseKind::kRequestRejected, code,
                                  "worker.request_rejected")};
    }
    return {WorkerExitCode::kHostFailure,
            boundary_response(job, contracts::WorkerResponseKind::kHostFailure,
                              base::ErrorCode::kInternal, "worker.host_failed")};
}

class PersonalBackupExecutor final : public IWorkerTaskExecutor {
  public:
    PersonalBackupExecutor(const contracts::JobRequest& job,
                           const WindowsPersonalBackupTaskOptions& options,
                           const WindowsPersonalBackupTaskContext& context)
        : job_(job), options_(options), context_(context) {}

    [[nodiscard]] base::Result<contracts::TaskResult>
    execute(const base::CancellationToken& cancellation) override {
        if (job_.operation == contracts::JobOperation::kVerify) {
            return execute_personal_archive_verify_task(job_, options_, context_, cancellation);
        }
        return execute_windows_personal_backup_task(job_, options_, context_, cancellation);
    }

  private:
    const contracts::JobRequest& job_;
    const WindowsPersonalBackupTaskOptions& options_;
    const WindowsPersonalBackupTaskContext& context_;
};

} // namespace

WorkerHostResult run_worker_host_with_executor(const contracts::JobRequest& job,
                                               const ports::IClock& clock,
                                               const base::CancellationToken& cancellation,
                                               IWorkerTaskExecutor& executor) {
    try {
        auto validation = contracts::validate_job_request(job);
        if (!validation) {
            return from_execution(job,
                                  base::Result<contracts::TaskResult>::failure(validation.error()));
        }
        const auto now_utc_ms = clock.now_utc_ms();
        if (now_utc_ms < 0) {
            return {WorkerExitCode::kHostFailure,
                    boundary_response(job, contracts::WorkerResponseKind::kHostFailure,
                                      base::ErrorCode::kInternal, "worker.clock_failed")};
        }
        DeadlineCancellation deadline(job.deadline_utc_ms, now_utc_ms, cancellation);
        auto result = from_execution(job, executor.execute(deadline.token()));
        const bool correlation_matches =
            result.response.job_id == job.job_id && result.response.trace_id == job.trace_id;
        if (!correlation_matches || !contracts::validate_worker_response(result.response)) {
            return {WorkerExitCode::kHostFailure,
                    boundary_response(job, contracts::WorkerResponseKind::kHostFailure,
                                      base::ErrorCode::kInternal, "worker.invalid_response")};
        }
        return result;
    } catch (...) {
        return {WorkerExitCode::kHostFailure,
                boundary_response(job, contracts::WorkerResponseKind::kHostFailure,
                                  base::ErrorCode::kInternal, "worker.host_failed")};
    }
}

} // namespace detail

WorkerHostResult run_windows_personal_backup_worker_host(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation) {
    detail::PersonalBackupExecutor executor(job, options, context);
    return detail::run_worker_host_with_executor(job, context.clock, cancellation, executor);
}

} // namespace aegra::apps::worker
