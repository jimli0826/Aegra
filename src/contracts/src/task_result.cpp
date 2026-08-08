#include "aegra/contracts/task_result.h"

#include <algorithm>

namespace aegra::contracts {
namespace {

bool is_known_outcome(const TaskOutcome outcome) noexcept {
    switch (outcome) {
    case TaskOutcome::kSucceeded:
    case TaskOutcome::kSucceededWithWarning:
    case TaskOutcome::kFailed:
    case TaskOutcome::kCancelled:
        return true;
    }
    return false;
}

bool has_empty_warning(const TaskResult& result) {
    return std::ranges::any_of(result.warning_codes,
                               [](const std::string& code) { return code.empty(); });
}

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_success(const TaskResult& result) {
    if (result.error_code != base::ErrorCode::kNone || !result.warning_codes.empty() ||
        result.partial_restore) {
        return invalid("successful task result cannot contain errors, warnings, or partial restore");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_warning(const TaskResult& result) {
    if (result.error_code != base::ErrorCode::kNone || result.warning_codes.empty()) {
        return invalid("warning task result requires a warning and no error");
    }
    if (result.partial_restore) {
        return validate_partial_restore_stats(*result.partial_restore);
    }
    return base::Result<void>::success();
}

base::Result<void> validate_cancelled(const TaskResult& result) {
    if (result.error_code != base::ErrorCode::kCancelled) {
        return invalid("cancelled task result requires cancelled error code");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_failure(const TaskResult& result) {
    if (result.error_code == base::ErrorCode::kNone ||
        result.error_code == base::ErrorCode::kCancelled) {
        return invalid("failed task result requires a non-cancellation error");
    }
    if (result.partial_restore) {
        return validate_partial_restore_stats(*result.partial_restore);
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_task_result(const TaskResult& result) {
    if (result.schema_version != kTaskResultSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported task result schema version",
        });
    }
    if (result.job_id.empty() || result.trace_id.empty() || result.message_code.empty()) {
        return invalid("task result correlation fields are required");
    }
    if (!is_known_outcome(result.outcome) || has_empty_warning(result)) {
        return invalid("task result outcome or warning code is invalid");
    }
    if (result.logical_bytes > kMaximumWireInteger || result.stored_bytes > kMaximumWireInteger ||
        result.chunk_count > kMaximumWireInteger || result.entry_count > kMaximumWireInteger ||
        result.stream_count > kMaximumWireInteger) {
        return invalid("task result integer exceeds wire range");
    }
    switch (result.outcome) {
    case TaskOutcome::kSucceeded:
        return validate_success(result);
    case TaskOutcome::kSucceededWithWarning:
        return validate_warning(result);
    case TaskOutcome::kFailed:
        return validate_failure(result);
    case TaskOutcome::kCancelled:
        return validate_cancelled(result);
    }
    return invalid("task result outcome is invalid");
}

} // namespace aegra::contracts
