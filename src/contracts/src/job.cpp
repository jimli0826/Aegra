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

bool is_known_backup_type(const BackupType type) noexcept {
    switch (type) {
    case BackupType::kFull:
    case BackupType::kIncremental:
    case BackupType::kDifferential:
        return true;
    }
    return false;
}

bool non_empty_refs(const std::vector<std::string>& refs) {
    return !refs.empty() &&
           !std::ranges::any_of(refs, [](const std::string& ref) { return ref.empty(); });
}

base::Result<void> validate_backup_options(const JobRequest& request) {
    if (request.operation != JobOperation::kBackup) {
        return request.backup ? invalid("backup options require a backup operation")
                              : base::Result<void>::success();
    }
    if (!request.backup || !is_known_backup_type(request.backup->type)) {
        return invalid("backup options are required and must have a known type");
    }
    if (request.content_kind == ContentKind::kFileSet) {
        if (request.backup->deduplication_enabled) {
            return invalid("file_set backup cannot enable deduplication");
        }
        if (request.backup->type != BackupType::kFull &&
            request.backup->type != BackupType::kIncremental) {
            return invalid("file_set backup allows full or incremental only");
        }
        // Parent archive path reuses parent_source_ref; volume parent credential is unused.
        if (!request.backup->parent_credential_ref.value.empty()) {
            return invalid("file_set backup must not set parent_credential_ref");
        }
        if (!request.backup->candidate_parent_uuid.empty() &&
            !base::is_canonical_uuid(request.backup->candidate_parent_uuid)) {
            return invalid("file_set candidate_parent_uuid is invalid");
        }
        if (request.backup->type == BackupType::kFull) {
            if (!request.backup->candidate_parent_uuid.empty() ||
                !request.backup->parent_source_ref.empty() || request.backup->service_full_reason) {
                return invalid("full file_set backup cannot carry parent or demotion fields");
            }
        } else if (request.backup->service_full_reason) {
            // Service catalog demotion: requested Incremental, forced Full before Worker checks.
            if (!is_known_incremental_downgrade_reason(*request.backup->service_full_reason) ||
                *request.backup->service_full_reason == IncrementalDowngradeReason::kNone) {
                return invalid("file_set service_full_reason is invalid");
            }
            if (!request.backup->candidate_parent_uuid.empty() ||
                !request.backup->parent_source_ref.empty()) {
                return invalid("service-demoted file_set backup cannot carry parent fields");
            }
        } else {
            // Viable Incremental: tip UUID + absolute parent archive path required.
            if (request.backup->candidate_parent_uuid.empty() ||
                request.backup->parent_source_ref.empty()) {
                return invalid("file_set incremental requires candidate parent and parent path");
            }
        }
        if (!request.backup->selection_fingerprint) {
            return invalid("file_set backup requires selection_fingerprint");
        }
        auto fingerprint =
            validate_file_selection_fingerprint(*request.backup->selection_fingerprint);
        if (!fingerprint) {
            return fingerprint;
        }
    } else {
        if (request.backup->selection_fingerprint ||
            !request.backup->candidate_parent_uuid.empty() || request.backup->service_full_reason) {
            return invalid("volume_set backup cannot carry file selection baseline fields");
        }
        const bool has_parent_source = !request.backup->parent_source_ref.empty();
        const bool has_parent_credential = !request.backup->parent_credential_ref.value.empty();
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
    // file_set Incremental keeps the schedule backup_set_uuid on the request; volume non-full
    // inherits from parent and must leave the field empty.
    if (request.content_kind == ContentKind::kVolumeSet &&
        request.backup->type != BackupType::kFull && !request.backup->backup_set_uuid.empty()) {
        return invalid("non-full backup inherits its backup set UUID from the parent");
    }
    if (request.content_kind == ContentKind::kFileSet &&
        request.backup->type == BackupType::kIncremental &&
        (!has_set || request.backup->backup_set_uuid == request.backup->file_uuid)) {
        return invalid("file_set incremental requires a distinct backup set UUID");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_volume_sources(const JobRequest& request) {
    if (!request.file_source_refs.empty() || request.file_restore_target) {
        return invalid("volume_set job cannot carry file payloads");
    }
    if (!non_empty_refs(request.source_refs) ||
        request.source_refs.size() > kMaximumFileSelections) {
        return invalid("volume source_refs are required");
    }
    if (request.operation == JobOperation::kVerify) {
        return request.target_ref.empty() ? base::Result<void>::success()
                                          : invalid("verify cannot have target_ref");
    }
    if (request.target_ref.empty()) {
        return invalid("target_ref is required");
    }
    if (request.operation == JobOperation::kRestore && request.restore) {
        return base::Result<void>::success();
    }
    if (request.operation == JobOperation::kRestore && !request.restore) {
        return base::Result<void>::success();
    }
    if (request.operation != JobOperation::kRestore && request.restore) {
        return invalid("restore options require a restore operation");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_sources(const JobRequest& request) {
    if (request.operation == JobOperation::kExport) {
        return invalid("file_set export is unsupported");
    }
    if (request.operation == JobOperation::kBackup) {
        if (!request.source_refs.empty() || request.file_restore_target || request.restore) {
            return invalid("file_set backup has mixed volume/file payload");
        }
        if (request.target_ref.empty()) {
            return invalid("file_set backup requires target_ref destination");
        }
        return validate_file_source_refs(request.file_source_refs);
    }
    if (request.operation == JobOperation::kVerify) {
        if (!request.file_source_refs.empty() || request.file_restore_target || request.restore ||
            !request.target_ref.empty()) {
            return invalid("file_set verify payload is invalid");
        }
        // Base-first Full..tip chain (1..kMaximumFileChainDepth). Composition supplies parents.
        if (!non_empty_refs(request.source_refs) ||
            request.source_refs.size() > kMaximumFileChainDepth) {
            return invalid("file_set verify requires base-first archive source_refs");
        }
        return base::Result<void>::success();
    }
    // restore
    if (!request.file_source_refs.empty() || request.restore || !request.target_ref.empty()) {
        return invalid("file_set restore has mixed volume/file payload");
    }
    // Base-first Full..tip chain; single Full remains size==1.
    if (!non_empty_refs(request.source_refs) ||
        request.source_refs.size() > kMaximumFileChainDepth) {
        return invalid("file_set restore requires base-first archive source_refs");
    }
    if (!request.file_restore_target) {
        return invalid("file_set restore requires file_restore_target");
    }
    return validate_file_restore_target(*request.file_restore_target);
}

} // namespace

base::Result<void> validate_job_request(const JobRequest& request) {
    if (request.schema_version != kJobSchemaVersion) {
        return base::Result<void>::failure(base::Error{
            base::ErrorCode::kUnsupportedVersion,
            "unsupported job schema version",
        });
    }
    if (request.job_id.empty() || request.tenant_id.empty() || request.trace_id.empty()) {
        return invalid("job correlation fields are required");
    }
    if (!is_known_operation(request.operation) || !is_known_content_kind(request.content_kind)) {
        return invalid("job operation or content_kind is invalid");
    }
    if (request.deadline_utc_ms < 0) {
        return invalid("deadline_utc_ms cannot be negative");
    }
    auto backup = validate_backup_options(request);
    if (!backup) {
        return backup;
    }
    if (request.content_kind == ContentKind::kVolumeSet) {
        return validate_volume_sources(request);
    }
    return validate_file_sources(request);
}

} // namespace aegra::contracts
