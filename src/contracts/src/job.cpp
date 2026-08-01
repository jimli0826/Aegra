#include "aegra/contracts/job.h"

#include <utility>

namespace aegra::contracts {
namespace {

base::Result<void> invalid(std::string message) {
    return base::Result<void>::failure(
        base::Error{base::ErrorCode::kInvalidArgument, std::move(message)});
}

} // namespace

base::Result<void> validate_job_request(const JobRequest& request) {
    if (request.schema_version != kJobSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported job schema version",
        });
    }
    if (request.job_id.empty()) {
        return invalid("job_id is required");
    }
    if (request.tenant_id.empty()) {
        return invalid("tenant_id is required");
    }
    if (request.source_refs.empty()) {
        return invalid("at least one source_ref is required");
    }
    if (request.target_ref.empty()) {
        return invalid("target_ref is required");
    }
    if (request.trace_id.empty()) {
        return invalid("trace_id is required");
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
