#include "aegra/contracts/job.h"

#include <algorithm>
#include <utility>

namespace aegra::contracts {
namespace {

base::Result<void> invalid(std::string message) {
    return base::Result<void>::failure(
        base::Error{base::ErrorCode::kInvalidArgument, std::move(message)});
}

bool is_known_operation(const JobOperation operation) noexcept {
    switch (operation) {
    case JobOperation::kBackup:
    case JobOperation::kRestore:
    case JobOperation::kVerify:
    case JobOperation::kExport:
        return true;
    }
    return false;
}

bool requires_target(const JobOperation operation) noexcept {
    return operation != JobOperation::kVerify;
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
    if (!is_known_operation(request.operation)) {
        return invalid("job operation is invalid");
    }
    if (request.source_refs.empty() ||
        std::ranges::any_of(request.source_refs,
                            [](const std::string& ref) { return ref.empty(); })) {
        return invalid("at least one source_ref is required");
    }
    if (requires_target(request.operation) && request.target_ref.empty()) {
        return invalid("target_ref is required");
    }
    if (request.trace_id.empty()) {
        return invalid("trace_id is required");
    }
    if (request.deadline_utc_ms < 0) {
        return invalid("deadline_utc_ms cannot be negative");
    }
    if (std::ranges::any_of(request.credential_refs,
                            [](const SecretRef& ref) { return ref.value.empty(); })) {
        return invalid("credential_ref cannot be empty");
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
