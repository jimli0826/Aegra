#include "aegra/apps/worker/worker_protocol.h"
#include "aegra/apps/worker/worker_session.h"

#include "worker_session_internal.h"

#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"
#include "aegra/ports/message_channel.h"
#include "aegra/ports/progress.h"

#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

std::string valid_job_json() {
    return R"({"schema_version":1,"job_id":"job-1","tenant_id":"tenant-1","operation":1,"source_refs":["source-1"],"target_ref":"target-1","credential_refs":["wincred://test"],"trace_id":"trace-1"})";
}

std::string cancel_json(const std::string_view job_id = "job-1") {
    return std::string(R"({"schema_version":1,"job_id":")") + std::string(job_id) +
           R"(","trace_id":"trace-1","kind":1})";
}

class MemoryChannel final : public aegra::ports::IMessageChannel {
  public:
    explicit MemoryChannel(std::vector<std::string> incoming)
        : incoming_(incoming.begin(), incoming.end()) {}

    [[nodiscard]] base::Result<std::string>
    receive(const base::CancellationToken& cancellation) override {
        std::unique_lock lock(mutex_);
        condition_.wait(lock, cancellation, [this] { return !incoming_.empty(); });
        if (incoming_.empty()) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kCancelled, "memory receive cancelled"});
        }
        auto value = std::move(incoming_.front());
        incoming_.pop_front();
        return base::Result<std::string>::success(std::move(value));
    }

    [[nodiscard]] base::Result<void> send(const std::string_view message,
                                          const base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "memory send cancelled"});
        }
        std::lock_guard lock(mutex_);
        sent_.emplace_back(message);
        return base::Result<void>::success();
    }

    [[nodiscard]] std::vector<std::string> sent() const {
        std::lock_guard lock(mutex_);
        return sent_;
    }

  private:
    mutable std::mutex mutex_;
    std::condition_variable_any condition_;
    std::deque<std::string> incoming_;
    std::vector<std::string> sent_;
};

contracts::TaskResult task_result(const contracts::TaskOutcome outcome) {
    contracts::TaskResult result;
    result.job_id = "job-1";
    result.trace_id = "trace-1";
    result.outcome = outcome;
    result.error_code = outcome == contracts::TaskOutcome::kCancelled ? base::ErrorCode::kCancelled
                                                                      : base::ErrorCode::kNone;
    result.message_code =
        outcome == contracts::TaskOutcome::kCancelled ? "backup.cancelled" : "backup.completed";
    return result;
}

app::WorkerHostResult host_result(const contracts::TaskOutcome outcome) {
    auto task = task_result(outcome);
    contracts::WorkerResponse response;
    response.job_id = task.job_id;
    response.trace_id = task.trace_id;
    response.kind = contracts::WorkerResponseKind::kTaskResult;
    response.boundary_error_code = base::ErrorCode::kNone;
    response.message_code = "worker.task_finished";
    response.task_result = std::move(task);
    const auto exit = outcome == contracts::TaskOutcome::kCancelled
                          ? app::WorkerExitCode::kCancelled
                          : app::WorkerExitCode::kSucceeded;
    return {exit, std::move(response)};
}

class TestRunner final : public detail::IWorkerSessionTaskRunner {
  public:
    explicit TestRunner(const bool wait_for_cancel) : wait_for_cancel_(wait_for_cancel) {}

    [[nodiscard]] app::WorkerHostResult run(const contracts::JobRequest& job,
                                            aegra::ports::IProgressSink& progress,
                                            const base::CancellationToken& cancellation) override {
        ++call_count;
        progress.publish(contracts::TaskProgress{
            contracts::kTaskProgressSchemaVersion,
            job.job_id,
            job.trace_id,
            contracts::TaskPhase::kPreparing,
            0,
            0,
            0,
            "backup.preparing",
        });
        while (wait_for_cancel_ && !cancellation.stop_requested()) {
            std::this_thread::yield();
        }
        return host_result(cancellation.stop_requested() ? contracts::TaskOutcome::kCancelled
                                                         : contracts::TaskOutcome::kSucceeded);
    }

    std::size_t call_count{0};

  private:
    bool wait_for_cancel_{false};
};

bool test_success_and_progress() {
    MemoryChannel channel({valid_job_json()});
    TestRunner runner(false);
    const auto exit = detail::run_worker_session_with_runner(channel, {}, runner);
    const auto sent = channel.sent();
    return expect(exit == app::WorkerExitCode::kSucceeded && runner.call_count == 1 &&
                      sent.size() == 2 &&
                      sent.front().find("backup.preparing") != std::string::npos &&
                      sent.back().find("worker.task_finished") != std::string::npos,
                  "session streams progress before one final successful result");
}

bool test_cancel_command() {
    MemoryChannel channel({valid_job_json(), cancel_json()});
    TestRunner runner(true);
    const auto exit = detail::run_worker_session_with_runner(channel, {}, runner);
    const auto sent = channel.sent();
    return expect(exit == app::WorkerExitCode::kCancelled && sent.size() == 2 &&
                      sent.back().find("backup.cancelled") != std::string::npos,
                  "correlated cancel command stops the running task");
}

bool test_bad_command_and_rejection() {
    MemoryChannel bad_command({valid_job_json(), cancel_json("wrong-job")});
    TestRunner waiting_runner(true);
    auto exit = detail::run_worker_session_with_runner(bad_command, {}, waiting_runner);
    auto sent = bad_command.sent();
    bool passed = expect(exit == app::WorkerExitCode::kHostFailure && sent.size() == 2 &&
                             sent.back().find("worker.command_failed") != std::string::npos,
                         "uncorrelated command becomes a stable host failure");

    MemoryChannel invalid_job({"{}"});
    TestRunner unused_runner(false);
    exit = detail::run_worker_session_with_runner(invalid_job, {}, unused_runner);
    sent = invalid_job.sent();
    passed &= expect(exit == app::WorkerExitCode::kRequestRejected &&
                         unused_runner.call_count == 0 && sent.size() == 1 &&
                         sent.front().find("worker.request_rejected") != std::string::npos,
                     "invalid initial Job is rejected without starting the task");
    return passed;
}

int run_tests() {
    return test_success_and_progress() && test_cancel_command() && test_bad_command_and_rejection()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
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
