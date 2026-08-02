#include "aegra/contracts/worker_session.h"

namespace aegra::contracts {
namespace {

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_progress_event(const WorkerEvent& event) {
    if (!event.progress || event.response) {
        return invalid("progress event requires only a progress payload");
    }
    auto validation = validate_task_progress(*event.progress);
    if (!validation) {
        return validation;
    }
    if (event.job_id != event.progress->job_id || event.trace_id != event.progress->trace_id) {
        return invalid("progress event correlation does not match its payload");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_result_event(const WorkerEvent& event) {
    if (event.progress || !event.response) {
        return invalid("result event requires only a response payload");
    }
    auto validation = validate_worker_response(*event.response);
    if (!validation) {
        return validation;
    }
    if (event.job_id != event.response->job_id || event.trace_id != event.response->trace_id) {
        return invalid("result event correlation does not match its payload");
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_worker_command(const WorkerCommand& command) {
    if (command.schema_version != kWorkerCommandSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported worker command schema version",
        });
    }
    if (command.job_id.empty() || command.trace_id.empty()) {
        return invalid("worker command correlation fields are required");
    }
    if (command.kind != WorkerCommandKind::kCancel) {
        return invalid("worker command kind is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_worker_event(const WorkerEvent& event) {
    if (event.schema_version != kWorkerEventSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported worker event schema version",
        });
    }
    switch (event.kind) {
    case WorkerEventKind::kProgress:
        return validate_progress_event(event);
    case WorkerEventKind::kResult:
        return validate_result_event(event);
    }
    return invalid("worker event kind is invalid");
}

} // namespace aegra::contracts
