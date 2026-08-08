#include "aegra/contracts/progress.h"

namespace aegra::contracts {
namespace {

bool is_known_phase(const TaskPhase phase) noexcept {
    switch (phase) {
    case TaskPhase::kUnspecified:
    case TaskPhase::kPreparing:
    case TaskPhase::kReading:
    case TaskPhase::kTransforming:
    case TaskPhase::kWriting:
    case TaskPhase::kCommitting:
    case TaskPhase::kCompleted:
        return true;
    }
    return false;
}

} // namespace

base::Result<void> validate_task_progress(const TaskProgress& progress) {
    if (progress.schema_version != kTaskProgressSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported task progress schema version",
        });
    }
    if (progress.job_id.empty() || progress.trace_id.empty()) {
        return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument,
                                                       "progress correlation fields are required"});
    }
    if (!is_known_phase(progress.phase)) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "task phase is invalid"});
    }
    if (progress.processed_bytes > kMaximumWireInteger ||
        progress.stored_bytes > kMaximumWireInteger ||
        progress.discovered_entries > kMaximumWireInteger ||
        progress.processed_entries > kMaximumWireInteger) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "progress integer exceeds wire range",
        });
    }
    if (progress.logical_bytes) {
        if (*progress.logical_bytes > kMaximumWireInteger) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "logical_bytes exceeds wire range",
            });
        }
        if (progress.processed_bytes > *progress.logical_bytes) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "processed_bytes exceeds logical_bytes",
            });
        }
        if (progress.phase == TaskPhase::kCompleted &&
            progress.processed_bytes != *progress.logical_bytes) {
            return base::Result<void>::failure(base::Error{
                base::ErrorCode::kInvalidArgument,
                "completed progress must cover all logical bytes",
            });
        }
    }
    if (progress.processed_entries > progress.discovered_entries &&
        progress.discovered_entries != 0) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "processed_entries exceeds discovered_entries",
        });
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
