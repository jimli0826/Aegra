#include "aegra/apps/boot_check/boot_check_host.h"

#include "boot_check_protocol.h"
#include "boot_check_task.h"

#include "aegra/contracts/boot_check_job.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <stop_token>
#include <thread>
#include <utility>

namespace aegra::apps::boot_check {
namespace {

/// Merges the external stop with the request deadline, mirroring the worker host.
class DeadlineCancellation final {
  public:
    DeadlineCancellation(const std::uint64_t deadline_utc_ms, const std::int64_t now_utc_ms,
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

    [[nodiscard]] base::CancellationToken token() const noexcept {
        return cancellation_.get_token();
    }

  private:
    struct ExternalCancellation final {
        base::CancellationSource* cancellation;

        void operator()() const noexcept { cancellation->request_stop(); }
    };

    void start_deadline(const std::uint64_t deadline_utc_ms, const std::int64_t now_utc_ms) {
        if (deadline_utc_ms == 0) {
            return;
        }
        if (deadline_utc_ms <= static_cast<std::uint64_t>(now_utc_ms)) {
            cancellation_.request_stop();
            return;
        }
        const auto remaining =
            std::chrono::milliseconds(deadline_utc_ms - static_cast<std::uint64_t>(now_utc_ms));
        watchdog_ = std::jthread([this, remaining] {
            std::unique_lock lock(mutex_);
            if (!changed_.wait_for(lock, remaining, [this] { return completed_; })) {
                cancellation_.request_stop();
            }
        });
    }

    base::CancellationSource cancellation_;
    std::stop_callback<ExternalCancellation> external_callback_;
    std::mutex mutex_;
    std::condition_variable changed_;
    bool completed_{false};
    std::jthread watchdog_;
};

[[nodiscard]] BootCheckExitCode exit_code_for(const contracts::TaskOutcome outcome) noexcept {
    switch (outcome) {
    case contracts::TaskOutcome::kSucceeded:
    case contracts::TaskOutcome::kSucceededWithWarning:
        return BootCheckExitCode::kSucceeded;
    case contracts::TaskOutcome::kFailed:
        return BootCheckExitCode::kTaskFailed;
    case contracts::TaskOutcome::kCancelled:
        return BootCheckExitCode::kCancelled;
    }
    return BootCheckExitCode::kHostFailure;
}

[[nodiscard]] contracts::WorkerResponse boundary_response(const std::string& job_id,
                                                          const std::string& trace_id,
                                                          const contracts::WorkerResponseKind kind,
                                                          const base::ErrorCode code,
                                                          const char* message_code) {
    contracts::WorkerResponse response;
    response.job_id = job_id;
    response.trace_id = trace_id;
    response.kind = kind;
    response.boundary_error_code = code;
    response.message_code = message_code;
    return response;
}

[[nodiscard]] base::Result<EncodedBootCheckResult>
encode_result(const BootCheckExitCode exit_code, const contracts::WorkerResponse& response) {
    if (!contracts::validate_worker_response(response)) {
        return base::Result<EncodedBootCheckResult>::failure(
            {base::ErrorCode::kInternal, "boot check response is invalid"});
    }
    auto encoded = detail::encode_boot_check_response(response);
    if (!encoded) {
        return base::Result<EncodedBootCheckResult>::failure(encoded.error());
    }
    return base::Result<EncodedBootCheckResult>::success({exit_code, std::move(encoded).value()});
}

[[nodiscard]] base::Result<EncodedBootCheckResult> rejection_result(const std::string& job_id,
                                                                    const std::string& trace_id,
                                                                    const base::ErrorCode code) {
    const auto boundary = code == base::ErrorCode::kUnsupportedVersion
                              ? base::ErrorCode::kUnsupportedVersion
                              : base::ErrorCode::kInvalidArgument;
    return encode_result(BootCheckExitCode::kRequestRejected,
                         boundary_response(job_id, trace_id,
                                           contracts::WorkerResponseKind::kRequestRejected,
                                           boundary, "bootcheck.request_rejected"));
}

[[nodiscard]] base::Result<EncodedBootCheckResult> host_failure_result(const std::string& job_id,
                                                                       const std::string& trace_id,
                                                                       const char* message_code) {
    return encode_result(BootCheckExitCode::kHostFailure,
                         boundary_response(job_id, trace_id,
                                           contracts::WorkerResponseKind::kHostFailure,
                                           base::ErrorCode::kInternal, message_code));
}

} // namespace

namespace {

template <typename TaskRunner>
[[nodiscard]] base::Result<EncodedBootCheckResult>
run_host_request(const std::string_view encoded_request, const BootCheckHostContext& context,
                 const base::CancellationToken& cancellation, TaskRunner&& run_task) {
    try {
        auto decoded = detail::decode_boot_check_job_request(encoded_request);
        if (!decoded) {
            return rejection_result({}, {}, decoded.error().code);
        }
        const auto request = std::move(decoded).value();
        if (auto valid = contracts::validate_boot_check_job_request(request); !valid) {
            return rejection_result(request.job_id, request.trace_id, valid.error().code);
        }
        const auto now_utc_ms = context.clock.now_utc_ms();
        if (now_utc_ms < 0) {
            return host_failure_result(request.job_id, request.trace_id, "bootcheck.clock_failed");
        }
        DeadlineCancellation deadline(request.deadline_utc_ms, now_utc_ms, cancellation);
        auto executed = run_task(request, deadline.token());
        if (!executed) {
            return host_failure_result(request.job_id, request.trace_id, "bootcheck.host_failed");
        }
        auto task_result = std::move(executed).value();
        const auto exit_code = exit_code_for(task_result.outcome);
        contracts::WorkerResponse response;
        response.job_id = task_result.job_id;
        response.trace_id = task_result.trace_id;
        response.kind = contracts::WorkerResponseKind::kTaskResult;
        response.boundary_error_code = base::ErrorCode::kNone;
        response.message_code = "bootcheck.task_finished";
        response.task_result = std::move(task_result);
        return encode_result(exit_code, response);
    } catch (...) {
        return host_failure_result({}, {}, "bootcheck.host_failed");
    }
}

} // namespace

base::Result<EncodedBootCheckResult> run_boot_check_host_request(
    const std::string_view encoded_request, const BootCheckHostOptions& options,
    const BootCheckHostContext& context, const base::CancellationToken& cancellation) {
    return run_host_request(
        encoded_request, context, cancellation,
        [&](const contracts::BootCheckJobRequest& request, const base::CancellationToken& token) {
            return detail::run_boot_check_task(request, options, context, token);
        });
}

base::Result<EncodedBootCheckResult> run_boot_check_present_request(
    const std::string_view encoded_request, const BootCheckHostOptions& options,
    const BootCheckHostContext& context, const std::uint32_t hold_minutes,
    const base::CancellationToken& cancellation) {
    const auto hold = std::chrono::minutes(hold_minutes == 0 ? 1 : hold_minutes);
    return run_host_request(
        encoded_request, context, cancellation,
        [&](const contracts::BootCheckJobRequest& request, const base::CancellationToken& token) {
            return detail::run_boot_check_present_hold(request, options, context, hold, token);
        });
}

} // namespace aegra::apps::boot_check
