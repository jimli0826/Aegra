#include "aegra/apps/worker/worker_host.h"

#include "worker_host_internal.h"

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/ports/clock.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {

namespace app = aegra::apps::worker;
namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace detail = aegra::apps::worker::detail;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class FixedClock final : public aegra::ports::IClock {
  public:
    explicit FixedClock(const std::int64_t now_utc_ms) : now_utc_ms_(now_utc_ms) {}

    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return now_utc_ms_; }

  private:
    std::int64_t now_utc_ms_;
};

contracts::JobRequest valid_job() {
    contracts::JobRequest job;
    job.job_id = "job-host-1";
    job.tenant_id = "tenant-1";
    job.source_refs = {"source-1"};
    job.target_ref = "target-1";
    job.trace_id = "trace-host-1";
    return job;
}

contracts::TaskResult task_result(const contracts::TaskOutcome outcome) {
    contracts::TaskResult result;
    result.job_id = "job-host-1";
    result.trace_id = "trace-host-1";
    result.outcome = outcome;
    if (outcome == contracts::TaskOutcome::kSucceeded) {
        result.error_code = base::ErrorCode::kNone;
        result.message_code = "backup.completed";
    } else if (outcome == contracts::TaskOutcome::kCancelled) {
        result.error_code = base::ErrorCode::kCancelled;
        result.message_code = "backup.cancelled";
    } else {
        result.error_code = base::ErrorCode::kIoFailure;
        result.message_code = "backup.failed";
    }
    return result;
}

class TestExecutor final : public detail::IWorkerTaskExecutor {
  public:
    using Action =
        std::function<base::Result<contracts::TaskResult>(const base::CancellationToken&)>;

    explicit TestExecutor(Action action) : action_(std::move(action)) {}

    [[nodiscard]] base::Result<contracts::TaskResult>
    execute(const base::CancellationToken& cancellation) override {
        ++call_count;
        return action_(cancellation);
    }

    std::size_t call_count{0};

  private:
    Action action_;
};

bool test_task_exit_mapping() {
    FixedClock clock{1'000};
    TestExecutor succeeded([](const base::CancellationToken&) {
        return base::Result<contracts::TaskResult>::success(
            task_result(contracts::TaskOutcome::kSucceeded));
    });
    auto result = detail::run_worker_host_with_executor(valid_job(), clock, {}, succeeded);
    bool passed = expect(result.exit_code == app::WorkerExitCode::kSucceeded,
                         "successful task maps to successful process exit");
    passed &= expect(result.response.kind == contracts::WorkerResponseKind::kTaskResult &&
                         result.response.task_result.has_value(),
                     "accepted task returns a task result response");

    TestExecutor failed([](const base::CancellationToken&) {
        return base::Result<contracts::TaskResult>::success(
            task_result(contracts::TaskOutcome::kFailed));
    });
    result = detail::run_worker_host_with_executor(valid_job(), clock, {}, failed);
    passed &= expect(result.exit_code == app::WorkerExitCode::kTaskFailed,
                     "task failure has a stable non-host exit code");
    return passed;
}

bool test_rejection_and_host_failure() {
    FixedClock clock{1'000};
    TestExecutor unused([](const base::CancellationToken&) {
        return base::Result<contracts::TaskResult>::success(
            task_result(contracts::TaskOutcome::kSucceeded));
    });
    auto invalid_job = valid_job();
    invalid_job.job_id.clear();
    auto rejected = detail::run_worker_host_with_executor(invalid_job, clock, {}, unused);
    bool passed =
        expect(rejected.exit_code == app::WorkerExitCode::kRequestRejected &&
                   rejected.response.kind == contracts::WorkerResponseKind::kRequestRejected &&
                   unused.call_count == 0,
               "invalid request is rejected before executor access");

    TestExecutor throws([](const base::CancellationToken&) -> base::Result<contracts::TaskResult> {
        throw std::runtime_error("secret backend detail");
    });
    auto failed = detail::run_worker_host_with_executor(valid_job(), clock, {}, throws);
    passed &= expect(failed.exit_code == app::WorkerExitCode::kHostFailure &&
                         failed.response.message_code == "worker.host_failed" &&
                         failed.response.message_code.find("secret") == std::string::npos,
                     "unexpected exceptions become a sanitized host failure");
    return passed;
}

bool test_external_and_deadline_cancellation() {
    FixedClock clock{1'000};
    base::CancellationSource external;
    TestExecutor external_wait([&external](const base::CancellationToken& cancellation) {
        external.request_stop();
        return base::Result<contracts::TaskResult>::success(
            task_result(cancellation.stop_requested() ? contracts::TaskOutcome::kCancelled
                                                      : contracts::TaskOutcome::kSucceeded));
    });
    auto cancelled = detail::run_worker_host_with_executor(valid_job(), clock, external.get_token(),
                                                           external_wait);
    bool passed = expect(cancelled.exit_code == app::WorkerExitCode::kCancelled,
                         "external process cancellation reaches the task token");

    auto deadline_job = valid_job();
    deadline_job.deadline_utc_ms = 1'020;
    TestExecutor deadline_wait([](const base::CancellationToken& cancellation) {
        const auto limit = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (!cancellation.stop_requested() && std::chrono::steady_clock::now() < limit) {
            std::this_thread::yield();
        }
        return base::Result<contracts::TaskResult>::success(
            task_result(cancellation.stop_requested() ? contracts::TaskOutcome::kCancelled
                                                      : contracts::TaskOutcome::kSucceeded));
    });
    cancelled = detail::run_worker_host_with_executor(deadline_job, clock, {}, deadline_wait);
    passed &= expect(cancelled.exit_code == app::WorkerExitCode::kCancelled,
                     "deadline cancels a task while it is running");
    return passed;
}

bool test_invalid_executor_result_is_contained() {
    FixedClock clock{1'000};
    TestExecutor invalid_result([](const base::CancellationToken&) {
        auto result = task_result(contracts::TaskOutcome::kSucceeded);
        result.trace_id = "wrong-trace";
        return base::Result<contracts::TaskResult>::success(std::move(result));
    });
    const auto result =
        detail::run_worker_host_with_executor(valid_job(), clock, {}, invalid_result);
    return expect(result.exit_code == app::WorkerExitCode::kHostFailure &&
                      result.response.message_code == "worker.invalid_response",
                  "invalid executor result cannot cross the process boundary");
}

bool test_invalid_clock_is_contained() {
    FixedClock invalid_clock{-1};
    TestExecutor unused([](const base::CancellationToken&) {
        return base::Result<contracts::TaskResult>::success(
            task_result(contracts::TaskOutcome::kSucceeded));
    });
    const auto result =
        detail::run_worker_host_with_executor(valid_job(), invalid_clock, {}, unused);
    return expect(result.exit_code == app::WorkerExitCode::kHostFailure &&
                      result.response.message_code == "worker.clock_failed" &&
                      unused.call_count == 0,
                  "invalid system time fails before the task starts");
}

int run_tests() {
    const bool passed = test_task_exit_mapping() && test_rejection_and_host_failure() &&
                        test_external_and_deadline_cancellation() &&
                        test_invalid_executor_result_is_contained() &&
                        test_invalid_clock_is_contained();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
