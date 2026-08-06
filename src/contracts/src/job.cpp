#include "aegra/contracts/job.h"

#include "aegra/base/uuid.h"

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

bool is_known_backup_type(const BackupType type) noexcept {
    switch (type) {
    case BackupType::kFull:
    case BackupType::kIncremental:
    case BackupType::kDifferential:
        return true;
    }
    return false;
}

base::Result<void> validate_backup_options(const JobRequest& request) {
    if (request.operation != JobOperation::kBackup) {
        return request.backup ? invalid("backup options require a backup operation")
                              : base::Result<void>::success();
    }
    if (!request.backup || !is_known_backup_type(request.backup->type)) {
        return invalid("backup options are required and must have a known type");
    }
    const bool has_parent_source = !request.backup->parent_source_ref.empty();
    const bool has_parent_credential = !request.backup->parent_credential_ref.value.empty();
    // Credential without a parent path is invalid; credential may be omitted when the Worker
    // reuses the archive password or the parent archive is unencrypted.
    if (has_parent_credential && !has_parent_source) {
        return invalid("backup parent credential requires a parent source");
    }
    if (request.backup->type == BackupType::kFull &&
        (has_parent_source || has_parent_credential)) {
        return invalid("full backup cannot have a parent");
    }
    if (request.backup->type != BackupType::kFull && !has_parent_source) {
        return invalid("non-full backup requires a parent source");
    }
    if (!base::is_canonical_uuid(request.backup->file_uuid) ||
        request.backup->created_utc_ms <= 0) {
        return invalid("backup identity and creation time are required");
    }
    const bool has_set = base::is_canonical_uuid(request.backup->backup_set_uuid);
    if (request.backup->type == BackupType::kFull &&
        (!has_set || request.backup->backup_set_uuid == request.backup->file_uuid)) {
        return invalid("full backup requires a distinct backup set UUID");
    }
    if (request.backup->type != BackupType::kFull && !request.backup->backup_set_uuid.empty()) {
        return invalid("non-full backup inherits its backup set UUID from the parent");
    }
    return base::Result<void>::success();
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
    return validate_backup_options(request);
}

} // namespace aegra::contracts
