#include "aegra/contracts/worker_response.h"

namespace aegra::contracts {
namespace {

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task_response(const WorkerResponse& response) {
    if (response.boundary_error_code != base::ErrorCode::kNone || !response.task_result) {
        return invalid("task response requires a result and no boundary error");
    }
    const auto& result = *response.task_result;
    auto validation = validate_task_result(result);
    if (!validation) {
        return validation;
    }
    if (response.job_id != result.job_id || response.trace_id != result.trace_id) {
        return invalid("worker response correlation does not match task result");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_rejection(const WorkerResponse& response) {
    if (response.task_result) {
        return invalid("rejected request cannot contain a task result");
    }
    if (response.boundary_error_code != base::ErrorCode::kInvalidArgument &&
        response.boundary_error_code != base::ErrorCode::kUnsupportedVersion) {
        return invalid("rejected request requires a schema or validation error");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_host_failure(const WorkerResponse& response) {
    if (response.task_result || response.boundary_error_code == base::ErrorCode::kNone ||
        response.boundary_error_code == base::ErrorCode::kInvalidArgument ||
        response.boundary_error_code == base::ErrorCode::kUnsupportedVersion) {
        return invalid("host failure requires a non-validation boundary error");
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_worker_response(const WorkerResponse& response) {
    if (response.schema_version != kWorkerResponseSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported worker response schema version",
        });
    }
    if (response.message_code.empty()) {
        return invalid("worker response message code is required");
    }
    switch (response.kind) {
    case WorkerResponseKind::kTaskResult:
        return validate_task_response(response);
    case WorkerResponseKind::kRequestRejected:
        return validate_rejection(response);
    case WorkerResponseKind::kHostFailure:
        return validate_host_failure(response);
    }
    return invalid("worker response kind is invalid");
}

} // namespace aegra::contracts
