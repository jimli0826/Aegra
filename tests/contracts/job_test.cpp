#include "aegra/contracts/job.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/task_result.h"
#include "aegra/contracts/worker_response.h"

#include <cstdio>
#include <cstdlib>

namespace {

aegra::contracts::JobRequest valid_request() {
    aegra::contracts::JobRequest request;
    request.job_id = "job-1";
    request.tenant_id = "tenant-1";
    request.source_refs = {"disk-0"};
    request.target_ref = "repository-1";
    request.trace_id = "trace-1";
    return request;
}

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_job_request_validation() {
    auto request = valid_request();
    bool passed = expect(aegra::contracts::validate_job_request(request).has_value(),
                         "valid job is accepted");

    request.schema_version = 99;
    const auto unsupported = aegra::contracts::validate_job_request(request);
    passed &= expect(!unsupported.has_value(), "unsupported schema is rejected");
    passed &= expect(unsupported.error().code == aegra::base::ErrorCode::kUnsupportedVersion,
                     "unsupported schema has stable error code");

    request = valid_request();
    request.job_id.clear();
    passed &= expect(!aegra::contracts::validate_job_request(request).has_value(),
                     "missing job id is rejected");
    request = valid_request();
    request.credential_refs = {aegra::contracts::SecretRef{}};
    passed &= expect(!aegra::contracts::validate_job_request(request).has_value(),
                     "empty credential reference is rejected");
    return passed;
}

bool test_progress_validation() {
    aegra::contracts::TaskProgress progress;
    progress.job_id = "job-1";
    progress.trace_id = "trace-1";
    progress.logical_bytes = 100;
    progress.processed_bytes = 40;
    bool passed = expect(aegra::contracts::validate_task_progress(progress).has_value(),
                         "valid progress is accepted");
    progress.processed_bytes = 101;
    passed &= expect(!aegra::contracts::validate_task_progress(progress).has_value(),
                     "progress beyond logical size is rejected");
    progress.processed_bytes = 99;
    progress.phase = aegra::contracts::TaskPhase::kCompleted;
    passed &= expect(!aegra::contracts::validate_task_progress(progress).has_value(),
                     "incomplete completed progress is rejected");
    progress.phase = aegra::contracts::TaskPhase::kUnspecified;
    passed &= expect(!aegra::contracts::validate_task_progress(progress).has_value(),
                     "unknown task phase is rejected");
    return passed;
}

bool test_task_result_validation() {
    aegra::contracts::TaskResult task_result;
    task_result.job_id = "job-1";
    task_result.trace_id = "trace-1";
    task_result.outcome = aegra::contracts::TaskOutcome::kSucceeded;
    task_result.error_code = aegra::base::ErrorCode::kNone;
    task_result.message_code = "backup.completed";
    bool passed = expect(aegra::contracts::validate_task_result(task_result).has_value(),
                         "valid task result is accepted");
    task_result.outcome = aegra::contracts::TaskOutcome::kSucceededWithWarning;
    passed &= expect(!aegra::contracts::validate_task_result(task_result).has_value(),
                     "warning result requires warning code");
    task_result.warning_codes = {"backup.snapshot_cleanup_failed"};
    passed &= expect(aegra::contracts::validate_task_result(task_result).has_value(),
                     "well-formed warning result is accepted");
    task_result.outcome = aegra::contracts::TaskOutcome::kCancelled;
    passed &= expect(!aegra::contracts::validate_task_result(task_result).has_value(),
                     "cancelled result requires cancelled error code");
    return passed;
}

bool test_worker_response_validation() {
    aegra::contracts::TaskResult task_result;
    task_result.job_id = "job-1";
    task_result.trace_id = "trace-1";
    task_result.outcome = aegra::contracts::TaskOutcome::kSucceeded;
    task_result.error_code = aegra::base::ErrorCode::kNone;
    task_result.message_code = "backup.completed";

    aegra::contracts::WorkerResponse response;
    response.job_id = "job-1";
    response.trace_id = "trace-1";
    response.kind = aegra::contracts::WorkerResponseKind::kTaskResult;
    response.boundary_error_code = aegra::base::ErrorCode::kNone;
    response.message_code = "worker.task_finished";
    response.task_result = task_result;
    bool passed = expect(aegra::contracts::validate_worker_response(response).has_value(),
                         "valid task response is accepted");

    response.trace_id = "wrong-trace";
    passed &= expect(!aegra::contracts::validate_worker_response(response).has_value(),
                     "task response correlation must match its result");
    response = {};
    response.kind = aegra::contracts::WorkerResponseKind::kRequestRejected;
    response.boundary_error_code = aegra::base::ErrorCode::kInvalidArgument;
    response.message_code = "worker.request_rejected";
    passed &= expect(aegra::contracts::validate_worker_response(response).has_value(),
                     "request rejection is valid without a task result");
    response.boundary_error_code = aegra::base::ErrorCode::kIoFailure;
    passed &= expect(!aegra::contracts::validate_worker_response(response).has_value(),
                     "request rejection only accepts validation errors");
    return passed;
}

int run_tests() {
    const bool passed = test_job_request_validation() && test_progress_validation() &&
                        test_task_result_validation() && test_worker_response_validation();
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
