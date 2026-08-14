#include "aegra/contracts/service_control.h"

#include "aegra/base/error.h"
#include "aegra/base/uuid.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::contracts {
namespace {

constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::size_t kMaximumLocatorBytes = 2'048;
constexpr std::size_t kMaximumMessageCodeBytes = 128;
constexpr std::size_t kMaximumMessageArguments = 16;
constexpr std::size_t kMaximumMessageArgumentBytes = 256;
constexpr std::size_t kMaximumCapabilities = 64;
constexpr std::size_t kMaximumCapabilityBytes = 64;
constexpr std::size_t kMaximumTimezoneBytes = 128;
constexpr std::size_t kMaximumMountPointBytes = 64;
constexpr std::size_t kMaximumTokenBytes = 1'024;

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool valid_stable_character(const unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
           value == '_' || value == '-' || value == ':';
}

[[nodiscard]] bool valid_stable_value(const std::string_view value,
                                      const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::ranges::all_of(value, valid_stable_character);
}

[[nodiscard]] bool valid_source_ids(const std::vector<std::string>& source_ids,
                                    const bool allow_empty) {
    if ((!allow_empty && source_ids.empty()) || source_ids.size() > kMaximumBackupSources) {
        return false;
    }
    std::set<std::string_view> seen;
    for (const auto& source_id : source_ids) {
        if (!valid_stable_value(source_id, kMaximumIdentifierBytes) ||
            !seen.insert(source_id).second) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool valid_text(const std::string_view value,
                              const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return character >= 0x20U && character != 0x7FU;
           });
}

[[nodiscard]] bool valid_token(const std::optional<std::string>& token) noexcept {
    if (!token) {
        return true;
    }
    return !token->empty() && token->size() <= kMaximumTokenBytes &&
           std::ranges::all_of(*token, [](const unsigned char character) {
               return character >= 0x21U && character <= 0x7EU;
           });
}

[[nodiscard]] bool known_backup_type(const BackupType type) noexcept {
    return type == BackupType::kFull || type == BackupType::kIncremental ||
           type == BackupType::kDifferential;
}

[[nodiscard]] bool known_job_operation(const JobOperation operation) noexcept {
    return operation == JobOperation::kBackup || operation == JobOperation::kRestore ||
           operation == JobOperation::kVerify || operation == JobOperation::kExport;
}

[[nodiscard]] bool terminal_job_state(const ServiceJobState state) noexcept {
    return state == ServiceJobState::kSucceeded || state == ServiceJobState::kFailed ||
           state == ServiceJobState::kCancelled || state == ServiceJobState::kInterrupted;
}

[[nodiscard]] bool known_job_state(const ServiceJobState state) noexcept {
    return state >= ServiceJobState::kQueued && state <= ServiceJobState::kInterrupted;
}

[[nodiscard]] bool known_repository_state(const RepositoryConnectionState state) noexcept {
    return state == RepositoryConnectionState::kAvailable ||
           state == RepositoryConnectionState::kUnavailable;
}

[[nodiscard]] bool known_audit_severity(const AuditSeverity severity) noexcept {
    return severity >= AuditSeverity::kInformation && severity <= AuditSeverity::kCritical;
}

[[nodiscard]] bool known_mount_state(const MountSessionState state) noexcept {
    return state >= MountSessionState::kMounting && state <= MountSessionState::kFailed;
}

[[nodiscard]] bool valid_wire_integer(const std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
}

[[nodiscard]] bool valid_optional_wire_integer(const std::optional<std::uint64_t>& value) noexcept {
    return !value || valid_wire_integer(*value);
}

template <typename Item, typename Validator>
[[nodiscard]] base::Result<void> validate_page(const ServicePage<Item>& page, Validator validator) {
    if (page.items.size() > kMaximumServicePageResults || !valid_token(page.continuation_token)) {
        return invalid("service page bounds are invalid");
    }
    for (const auto& item : page.items) {
        auto valid = validator(item);
        if (!valid) {
            return valid;
        }
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_message_arguments(const MessageArguments& arguments) {
    if (arguments.size() > kMaximumMessageArguments) {
        return invalid("too many message arguments");
    }
    std::string_view previous;
    for (const auto& argument : arguments) {
        if (!valid_stable_value(argument.name, kMaximumCapabilityBytes) ||
            !valid_text(argument.value, kMaximumMessageArgumentBytes) ||
            (!previous.empty() && argument.name <= previous)) {
            return invalid("message arguments are invalid or unsorted");
        }
        previous = argument.name;
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_version_range(const ServiceVersionRange& range) {
    if (range.minimum_api_version == 0 || range.minimum_api_version > range.maximum_api_version) {
        return invalid("service version range is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_page_request(const ServicePageRequest& request) {
    if (request.maximum_results == 0 || request.maximum_results > kMaximumServicePageResults ||
        !valid_token(request.continuation_token)) {
        return invalid("service page request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_service_recovery_point_list_request(const ServiceRecoveryPointListRequest& request) {
    if (request.repository_connection_id &&
        !valid_stable_value(*request.repository_connection_id, kMaximumIdentifierBytes)) {
        return invalid("recovery point repository connection is invalid");
    }
    return validate_recovery_point_list_request(request.page);
}

base::Result<void> validate_service_recovery_point_page(const ServiceRecoveryPointPage& page) {
    if (page.repository_connection_id &&
        !valid_stable_value(*page.repository_connection_id, kMaximumIdentifierBytes)) {
        return invalid("recovery point page repository connection is invalid");
    }
    return validate_recovery_point_page(page.catalog);
}

base::Result<void>
validate_repository_connection_list_request(const RepositoryConnectionListRequest& request) {
    auto valid_page = validate_service_page_request(request.page);
    if (!valid_page || (request.state && !known_repository_state(*request.state))) {
        return invalid("repository connection list request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_repository_connection_summary(const RepositoryConnectionSummary& summary) {
    if (!valid_stable_value(summary.connection_id, kMaximumIdentifierBytes) ||
        !valid_text(summary.display_name, kMaximumDisplayNameBytes) ||
        !valid_text(summary.locator, kMaximumLocatorBytes) ||
        !known_repository_state(summary.state) ||
        summary.capabilities.size() > kMaximumCapabilities) {
        return invalid("repository connection summary is invalid");
    }
    std::string_view previous;
    for (const auto& capability : summary.capabilities) {
        if (!valid_stable_value(capability, kMaximumCapabilityBytes) ||
            (!previous.empty() && capability <= previous)) {
            return invalid("repository capabilities are invalid or unsorted");
        }
        previous = capability;
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_source_inventory_list_request(const SourceInventoryListRequest& request) {
    return validate_service_page_request(request.page);
}

base::Result<void> validate_source_inventory_item(const SourceInventoryItem& item) {
    const auto known_kind = item.kind == SourceKind::kVolume;
    const auto known_state = item.availability == SourceAvailability::kAvailable ||
                             item.availability == SourceAvailability::kUnavailable;
    if (!valid_stable_value(item.source_id, kMaximumIdentifierBytes) ||
        !valid_text(item.display_name, kMaximumDisplayNameBytes) || !known_kind || !known_state ||
        !valid_wire_integer(item.capacity_bytes) || !valid_wire_integer(item.free_bytes) ||
        item.free_bytes > item.capacity_bytes || !valid_wire_integer(item.disk_capacity_bytes) ||
        (item.is_selectable && item.availability != SourceAvailability::kAvailable) ||
        !valid_wire_integer(static_cast<std::uint64_t>(item.disk_number)) ||
        item.mount_letter.size() > 16 ||
        (!item.mount_letter.empty() && !valid_text(item.mount_letter, 16)) ||
        item.volume_label.size() > kMaximumDisplayNameBytes ||
        (!item.volume_label.empty() && !valid_text(item.volume_label, kMaximumDisplayNameBytes)) ||
        !valid_text(item.health_status, kMaximumDisplayNameBytes) ||
        !valid_text(item.partition_style, 32) || !valid_text(item.media_type, 32)) {
        return invalid("source inventory item is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_job_list_request(const JobListRequest& request) {
    auto valid_page = validate_service_page_request(request.page);
    if (!valid_page || (request.operation && !known_job_operation(*request.operation)) ||
        (request.state && !known_job_state(*request.state))) {
        return invalid("job list request is invalid");
    }
    if (request.scope != JobListScope::kAll && request.scope != JobListScope::kActive &&
        request.scope != JobListScope::kTerminal) {
        return invalid("job list request is invalid");
    }
    if (request.state) {
        const auto terminal = terminal_job_state(*request.state);
        if (request.scope == JobListScope::kActive && terminal) {
            return invalid("job list request is invalid");
        }
        if (request.scope == JobListScope::kTerminal && !terminal) {
            return invalid("job list request is invalid");
        }
    }
    if ((request.from_utc_ms && !valid_wire_integer(*request.from_utc_ms)) ||
        (request.to_utc_ms && !valid_wire_integer(*request.to_utc_ms)) ||
        (request.from_utc_ms && request.to_utc_ms && *request.from_utc_ms > *request.to_utc_ms)) {
        return invalid("job list request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_job_summary(const JobSummary& summary) {
    if (summary.content_kind && !is_known_content_kind(*summary.content_kind)) {
        return invalid("job summary content_kind is invalid");
    }
    if (!valid_stable_value(summary.job_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(summary.trace_id, kMaximumIdentifierBytes) ||
        !known_job_operation(summary.operation) ||
        !valid_stable_value(summary.message_code, kMaximumMessageCodeBytes)) {
        return invalid("job summary identity or operation is invalid");
    }
    if (!known_job_state(summary.state) || !valid_wire_integer(summary.created_utc_ms) ||
        !valid_optional_wire_integer(summary.started_utc_ms) ||
        !valid_optional_wire_integer(summary.completed_utc_ms) ||
        (summary.started_utc_ms && *summary.started_utc_ms < summary.created_utc_ms) ||
        (summary.completed_utc_ms &&
         (!summary.started_utc_ms || *summary.completed_utc_ms < *summary.started_utc_ms)) ||
        terminal_job_state(summary.state) != summary.completed_utc_ms.has_value()) {
        return invalid("job summary state or timestamps are invalid");
    }
    if (summary.progress) {
        auto valid = validate_task_progress(*summary.progress);
        if (!valid || summary.progress->job_id != summary.job_id ||
            summary.progress->trace_id != summary.trace_id ||
            !valid_stable_value(summary.progress->message_code, kMaximumMessageCodeBytes)) {
            return invalid("job summary progress is invalid");
        }
    }
    if (!valid_source_ids(summary.source_ids, true) ||
        (summary.repository_connection_id &&
         !valid_stable_value(*summary.repository_connection_id, kMaximumIdentifierBytes)) ||
        (summary.schedule_id &&
         !valid_stable_value(*summary.schedule_id, kMaximumIdentifierBytes))) {
        return invalid("job summary source, schedule, or repository connection is invalid");
    }
    if (summary.operation == JobOperation::kBackup) {
        if (!summary.schedule_id || summary.schedule_id->empty()) {
            return invalid("backup job summary requires schedule_id");
        }
    } else if (summary.schedule_id && !summary.schedule_id->empty()) {
        return invalid("non-backup job summary must not set schedule_id");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_schedule_trigger(const ScheduleTrigger& trigger) {
    const auto known_kind = trigger.kind == ScheduleTriggerKind::kDaily ||
                            trigger.kind == ScheduleTriggerKind::kWeekly ||
                            trigger.kind == ScheduleTriggerKind::kMonthly;
    if (!known_kind || trigger.local_minutes_of_day.empty() ||
        trigger.local_minutes_of_day.size() > kMaximumLocalMinutesOfDay ||
        !valid_text(trigger.timezone_id, kMaximumTimezoneBytes) ||
        (trigger.day_of_month_mask & ~kMaximumDayOfMonthMask) != 0) {
        return invalid("schedule trigger is invalid");
    }
    for (std::size_t index = 0; index < trigger.local_minutes_of_day.size(); ++index) {
        const auto minute = trigger.local_minutes_of_day[index];
        if (minute >= 24U * 60U) {
            return invalid("schedule trigger is invalid");
        }
        if (index > 0) {
            const auto previous = trigger.local_minutes_of_day[index - 1];
            if (previous >= minute) {
                return invalid("schedule trigger times must be sorted unique");
            }
            if (static_cast<std::uint16_t>(minute - previous) < kMinimumLocalMinutesOfDayGap) {
                return invalid("schedule trigger times are too close");
            }
        }
    }
    if (trigger.local_minutes_of_day.size() >= 2) {
        const auto first = trigger.local_minutes_of_day.front();
        const auto last = trigger.local_minutes_of_day.back();
        const auto wrap_gap =
            static_cast<std::uint16_t>((first + 24U * 60U) - last);
        if (wrap_gap < kMinimumLocalMinutesOfDayGap) {
            return invalid("schedule trigger times are too close");
        }
    }
    if (trigger.kind == ScheduleTriggerKind::kDaily &&
        (trigger.weekday_mask != 0 || trigger.day_of_month_mask != 0)) {
        return invalid("schedule weekday selection is invalid");
    }
    if (trigger.kind == ScheduleTriggerKind::kWeekly &&
        (trigger.weekday_mask == 0 || (trigger.weekday_mask & 0x80U) != 0 ||
         trigger.day_of_month_mask != 0)) {
        return invalid("schedule weekday selection is invalid");
    }
    if (trigger.kind == ScheduleTriggerKind::kMonthly &&
        (trigger.weekday_mask != 0 || trigger.day_of_month_mask == 0)) {
        return invalid("schedule month-day selection is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_schedule_summary(const ScheduleSummary& summary) {
    auto valid_trigger = validate_schedule_trigger(summary.trigger);
    if (!valid_stable_value(summary.schedule_id, kMaximumIdentifierBytes) ||
        !valid_text(summary.display_name, kMaximumDisplayNameBytes) ||
        !is_known_content_kind(summary.content_kind) ||
        !valid_stable_value(summary.repository_connection_id, kMaximumIdentifierBytes) ||
        !known_backup_type(summary.backup_type) || !valid_trigger ||
        !valid_optional_wire_integer(summary.next_run_utc_ms)) {
        return invalid("schedule summary is invalid");
    }
    if (summary.content_kind == ContentKind::kVolumeSet) {
        if (!valid_source_ids(summary.source_ids, false) || !summary.selection_summaries.empty()) {
            return invalid("volume schedule summary sources are invalid");
        }
    } else {
        if (summary.deduplication_enabled) {
            return invalid("file schedule cannot enable deduplication");
        }
        if (!summary.source_ids.empty() || summary.selection_summaries.empty() ||
            summary.selection_summaries.size() > kMaximumFileSelections ||
            (summary.backup_type != BackupType::kFull &&
             summary.backup_type != BackupType::kIncremental)) {
            return invalid("file schedule summary selections are invalid");
        }
        std::set<std::string_view> seen;
        for (const auto& item : summary.selection_summaries) {
            if (!base::is_canonical_uuid(item.selection_id) ||
                !valid_text(item.display_label, kMaximumDisplayLabelBytes) ||
                !is_known_file_entry_kind(item.entry_kind) ||
                !is_known_file_recursion(item.recursion) || !seen.insert(item.selection_id).second ||
                item.display_chain.empty() ||
                item.display_chain.size() > kMaximumRelativePathComponents) {
                return invalid("file schedule selection summary is invalid");
            }
            for (const auto& part : item.display_chain) {
                if (!valid_text(part, kMaximumDisplayLabelBytes)) {
                    return invalid("file schedule selection summary is invalid");
                }
            }
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_schedule_list_request(const ScheduleListRequest& request) {
    return validate_service_page_request(request.page);
}

base::Result<void> validate_audit_event_list_request(const AuditEventListRequest& request) {
    auto valid_page = validate_service_page_request(request.page);
    if (!valid_page ||
        (request.minimum_severity && !known_audit_severity(*request.minimum_severity)) ||
        !valid_optional_wire_integer(request.from_utc_ms) ||
        !valid_optional_wire_integer(request.to_utc_ms) ||
        (request.from_utc_ms && request.to_utc_ms && *request.from_utc_ms > *request.to_utc_ms) ||
        (request.correlation_id &&
         !valid_stable_value(*request.correlation_id, kMaximumIdentifierBytes))) {
        return invalid("audit event list request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_audit_event_summary(const AuditEventSummary& summary) {
    auto valid_arguments = validate_message_arguments(summary.message_arguments);
    if (!valid_stable_value(summary.event_id, kMaximumIdentifierBytes) ||
        !valid_wire_integer(summary.created_utc_ms) || !known_audit_severity(summary.severity) ||
        !valid_stable_value(summary.message_code, kMaximumMessageCodeBytes) || !valid_arguments ||
        !valid_stable_value(summary.correlation_id, kMaximumIdentifierBytes)) {
        return invalid("audit event summary is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_mount_session_list_request(const MountSessionListRequest& request) {
    auto valid_page = validate_service_page_request(request.page);
    if (!valid_page || (request.state && !known_mount_state(*request.state))) {
        return invalid("mount session list request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_mount_session_summary(const MountSessionSummary& summary) {
    // mount_point may be empty while mounting or when no drive letter was assigned yet.
    if (!valid_stable_value(summary.session_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(summary.recovery_point_id, kMaximumIdentifierBytes) ||
        !known_mount_state(summary.state) ||
        summary.mount_point.size() > kMaximumMountPointBytes ||
        (!summary.mount_point.empty() &&
         !valid_text(summary.mount_point, kMaximumMountPointBytes)) ||
        !valid_wire_integer(summary.source_disk_number) ||
        !valid_wire_integer(summary.disk_size_bytes) ||
        !valid_wire_integer(summary.started_utc_ms) ||
        !valid_stable_value(summary.message_code, kMaximumMessageCodeBytes)) {
        return invalid("mount session summary is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_repository_connection_input(const RepositoryConnectionInput& input) {
    constexpr std::size_t kMaximumNetworkFieldBytes = 256;
    if (!valid_text(input.display_name, kMaximumDisplayNameBytes) ||
        !valid_text(input.locator, kMaximumLocatorBytes) ||
        (input.credential_ref && !valid_text(input.credential_ref->value, kMaximumLocatorBytes)) ||
        input.network_username.size() > kMaximumNetworkFieldBytes ||
        input.network_password.size() > kMaximumNetworkFieldBytes ||
        input.network_domain.size() > kMaximumNetworkFieldBytes) {
        return invalid("repository connection input is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_resource_ref(const ResourceRef& reference) {
    return valid_stable_value(reference.resource_id, kMaximumIdentifierBytes)
               ? base::Result<void>::success()
               : invalid("resource reference is invalid");
}

base::Result<void> validate_recovery_point_ref(const RecoveryPointRef& reference) {
    if (!valid_stable_value(reference.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(reference.recovery_point_id, kMaximumIdentifierBytes)) {
        return invalid("recovery point reference is invalid");
    }
    constexpr std::size_t kMaximumArchivePasswordBytes = 32;
    if (reference.archive_password.size() > kMaximumArchivePasswordBytes) {
        return invalid("recovery point archive password is too long");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_start_backup_command(const StartBackupCommand& command) {
    // Wire payload is only schedule_id + backup_type; Service expands the rest from SQLite.
    if (!valid_stable_value(command.schedule_id, kMaximumIdentifierBytes) ||
        !known_backup_type(command.backup_type) ||
        command.backup_type == BackupType::kDifferential) {
        return invalid("start backup command is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_start_verify_command(const StartVerifyCommand& command) {
    if (!valid_stable_value(command.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(command.recovery_point_id, kMaximumIdentifierBytes)) {
        return invalid("start verify command is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_restore_preflight_request(const RestorePreflightRequest& request) {
    if (!valid_stable_value(request.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(request.recovery_point_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(request.target_source_id, kMaximumIdentifierBytes) ||
        request.archive_password.size() > 32) {
        return invalid("restore preflight request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_restore_preflight(const RestorePreflight& preflight) {
    if (!valid_token(preflight.preflight_token) ||
        !valid_stable_value(preflight.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(preflight.recovery_point_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(preflight.target_source_id, kMaximumIdentifierBytes) ||
        preflight.logical_size_bytes == 0 || !valid_wire_integer(preflight.logical_size_bytes) ||
        preflight.target_capacity_bytes < preflight.logical_size_bytes ||
        !valid_wire_integer(preflight.target_capacity_bytes) || preflight.chain_depth == 0 ||
        preflight.expires_utc_ms == 0 || !valid_wire_integer(preflight.expires_utc_ms) ||
        !preflight.restore_eligible ||
        !valid_stable_value(preflight.message_code, kMaximumMessageCodeBytes)) {
        return invalid("restore preflight is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_start_restore_command(const StartRestoreCommand& command) {
    const std::optional<std::string> token = command.preflight_token;
    if (!valid_token(token) || !command.confirmed || command.archive_password.size() > 32) {
        return invalid("start restore command is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_mount_recovery_point_command(const MountRecoveryPointCommand& command) {
    constexpr std::size_t kMaximumArchivePasswordBytes = 32;
    if (!valid_stable_value(command.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(command.recovery_point_id, kMaximumIdentifierBytes) ||
        command.archive_password.size() > kMaximumArchivePasswordBytes) {
        return invalid("mount recovery point command is invalid");
    }
    if (command.preferred_drive_letter && (command.preferred_drive_letter->size() != 1 ||
                                           command.preferred_drive_letter->front() < 'A' ||
                                           command.preferred_drive_letter->front() > 'Z')) {
        return invalid("preferred drive letter is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_upsert_schedule_command(const UpsertScheduleCommand& command) {
    if ((command.schedule_id &&
         !valid_stable_value(*command.schedule_id, kMaximumIdentifierBytes)) ||
        !valid_text(command.display_name, kMaximumDisplayNameBytes) ||
        !valid_stable_value(command.repository_connection_id, kMaximumIdentifierBytes) ||
        !known_backup_type(command.backup_type)) {
        return invalid("upsert schedule command is invalid");
    }
    if (command.protection.content_kind == ContentKind::kFileSet) {
        if (command.deduplication_enabled) {
            return invalid("file_set schedule cannot enable deduplication");
        }
        if (command.backup_type != BackupType::kFull &&
            command.backup_type != BackupType::kIncremental) {
            return invalid("file_set schedule requires full or incremental backup type");
        }
    }
    auto protection = validate_protection_spec_input(command.protection, !command.schedule_id);
    if (!protection) {
        return protection;
    }
    constexpr std::size_t kMaximumArchivePasswordBytes = 32;
    if (command.schedule_id && !command.archive_password.empty()) {
        return invalid("schedule password cannot be changed after create");
    }
    if (command.encryption_enabled) {
        if (!command.schedule_id &&
            (command.archive_password.empty() ||
             command.archive_password.size() > kMaximumArchivePasswordBytes)) {
            return invalid("encrypted schedule requires a password of 1 to 32 characters");
        }
    } else if (!command.archive_password.empty()) {
        return invalid("unencrypted schedule must not include a password");
    }
    return validate_schedule_trigger(command.trigger);
}

base::Result<void> validate_event_subscription_request(const EventSubscriptionRequest& request) {
    if (!valid_token(request.resume_token) || request.maximum_unacknowledged_events == 0 ||
        request.maximum_unacknowledged_events > kMaximumUnacknowledgedServiceEvents ||
        (!request.resume_token && request.after_sequence != 0)) {
        return invalid("event subscription request is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_event_acknowledgement(const EventAcknowledgement& acknowledgement) {
    if (!valid_stable_value(acknowledgement.subscription_id, kMaximumIdentifierBytes) ||
        acknowledgement.through_sequence == 0) {
        return invalid("event acknowledgement is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_command_acknowledgement(const CommandAcknowledgement& acknowledgement) {
    const auto known_disposition = acknowledgement.disposition == CommandDisposition::kAccepted ||
                                   acknowledgement.disposition == CommandDisposition::kReplayed;
    if (!valid_stable_value(acknowledgement.command_id, kMaximumIdentifierBytes) ||
        !known_disposition ||
        (acknowledgement.resource_id &&
         !valid_stable_value(*acknowledgement.resource_id, kMaximumIdentifierBytes))) {
        return invalid("command acknowledgement is invalid");
    }
    if (!acknowledgement.event_subscription) {
        return base::Result<void>::success();
    }
    const auto& lease = *acknowledgement.event_subscription;
    const std::optional<std::string> token = lease.resume_token;
    if (!valid_stable_value(lease.subscription_id, kMaximumIdentifierBytes) ||
        !valid_token(token) || lease.next_sequence == 0 ||
        lease.maximum_unacknowledged_events == 0 ||
        lease.maximum_unacknowledged_events > kMaximumUnacknowledgedServiceEvents) {
        return invalid("event subscription lease is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_repository_connection_page(const RepositoryConnectionPage& page) {
    return validate_page(page, validate_repository_connection_summary);
}

base::Result<void> validate_source_inventory_page(const SourceInventoryPage& page) {
    return validate_page(page, validate_source_inventory_item);
}

base::Result<void> validate_job_page(const JobPage& page) {
    return validate_page(page, validate_job_summary);
}

base::Result<void> validate_schedule_page(const SchedulePage& page) {
    return validate_page(page, validate_schedule_summary);
}

base::Result<void> validate_audit_event_page(const AuditEventPage& page) {
    return validate_page(page, validate_audit_event_summary);
}

base::Result<void> validate_mount_session_page(const MountSessionPage& page) {
    return validate_page(page, validate_mount_session_summary);
}

base::Result<void> validate_recovery_point_layout(const RecoveryPointLayout& layout) {
    if (!valid_stable_value(layout.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(layout.recovery_point_id, kMaximumIdentifierBytes) ||
        layout.disks.empty() || layout.disks.size() > 64 || layout.volumes.empty() ||
        layout.volumes.size() > 100) {
        return invalid("recovery point layout is invalid");
    }
    std::set<std::uint32_t> seen_disks;
    std::set<std::pair<std::uint32_t, std::uint32_t>> known_partitions;
    for (const auto& disk : layout.disks) {
        if (!seen_disks.insert(disk.disk_number).second || disk.disk_size_bytes == 0 ||
            (disk.partition_style != "mbr" && disk.partition_style != "gpt" &&
             disk.partition_style != "raw") ||
            disk.model.size() > 256 || disk.media_type.size() > 64 ||
            disk.partitions.size() > 256) {
            return invalid("recovery point source disk is invalid");
        }
        std::set<std::uint32_t> seen_parts;
        for (const auto& partition : disk.partitions) {
            if (!seen_parts.insert(partition.partition_number).second ||
                partition.size_bytes == 0 || partition.gpt_type_guid.size() > 64 ||
                partition.gpt_name.size() > 256 || partition.volume_label.size() > 256 ||
                partition.filesystem.size() > 64) {
                return invalid("recovery point source partition is invalid");
            }
            known_partitions.emplace(disk.disk_number, partition.partition_number);
        }
    }
    std::set<std::uint32_t> seen_index;
    for (const auto& volume : layout.volumes) {
        if (!seen_index.insert(volume.volume_index).second || volume.total_size_bytes == 0 ||
            volume.letter.size() > 16 || volume.label.size() > 256 ||
            volume.filesystem.size() > 64 || volume.extents.empty() ||
            volume.extents.size() > 32) {
            return invalid("recovery point source volume is invalid");
        }
        for (const auto& extent : volume.extents) {
            if (extent.length == 0 ||
                !known_partitions.contains({extent.disk_number, extent.partition_number})) {
                return invalid("recovery point source volume extent is invalid");
            }
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_recovery_point_chain_result(const RecoveryPointChainResult& result) {
    if (!valid_stable_value(result.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(result.recovery_point_id, kMaximumIdentifierBytes) ||
        result.layers.empty() || result.layers.size() > 128 ||
        !valid_stable_value(result.message_code, kMaximumMessageCodeBytes)) {
        return invalid("recovery point chain result is invalid");
    }
    if (result.layers.back().recovery_point_id != result.recovery_point_id) {
        return invalid("recovery point chain leaf mismatch");
    }
    std::set<std::string, std::less<>> seen;
    for (std::size_t index = 0; index < result.layers.size(); ++index) {
        const auto& layer = result.layers[index];
        if (!valid_stable_value(layer.recovery_point_id, kMaximumIdentifierBytes) ||
            !known_backup_type(layer.backup_type) ||
            (layer.parent_recovery_point_id &&
             !valid_stable_value(*layer.parent_recovery_point_id, kMaximumIdentifierBytes)) ||
            layer.structural_state < RecoveryPointStructuralState::kComplete ||
            layer.structural_state > RecoveryPointStructuralState::kCorrupt ||
            layer.authentication_state < RecoveryPointAuthenticationState::kNotAttempted ||
            layer.authentication_state > RecoveryPointAuthenticationState::kCredentialRequired ||
            layer.chain_state < RecoveryPointChainCompleteness::kComplete ||
            layer.chain_state > RecoveryPointChainCompleteness::kInvalid ||
            !seen.insert(layer.recovery_point_id).second) {
            return invalid("recovery point chain layer is invalid");
        }
        if (index == 0) {
            if (layer.backup_type != BackupType::kFull || layer.parent_recovery_point_id) {
                return invalid("recovery point chain base layer is invalid");
            }
        } else {
            const auto& parent = result.layers[index - 1];
            if (!layer.parent_recovery_point_id ||
                *layer.parent_recovery_point_id != parent.recovery_point_id) {
                return invalid("recovery point chain parent link is invalid");
            }
        }
    }
    const bool structurally_ready = std::ranges::all_of(result.layers, [](const auto& layer) {
        return layer.structural_state == RecoveryPointStructuralState::kComplete;
    });
    const bool chain_ready = std::ranges::all_of(result.layers, [](const auto& layer) {
        return layer.chain_state == RecoveryPointChainCompleteness::kComplete;
    });
    const bool authenticated = std::ranges::all_of(result.layers, [](const auto& layer) {
        return layer.authentication_state == RecoveryPointAuthenticationState::kAuthenticated;
    });
    if (result.restore_eligible && !(structurally_ready && chain_ready && authenticated)) {
        return invalid("restore eligibility is inconsistent with chain layer state");
    }
    if (result.mount_eligible && !(structurally_ready && chain_ready && authenticated)) {
        return invalid("mount eligibility is inconsistent with chain layer state");
    }
    if (result.verify_eligible && !structurally_ready) {
        return invalid("verify eligibility is inconsistent with structural state");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_delete_plan_summary(const DeletePlanSummary& summary) {
    if (!valid_token(summary.plan_token) ||
        !valid_stable_value(summary.operation_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(summary.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(summary.root_recovery_point_id, kMaximumIdentifierBytes) ||
        summary.targets.empty() || summary.targets.size() > 10'000 || summary.expires_utc_ms == 0 ||
        !valid_wire_integer(summary.expires_utc_ms)) {
        return invalid("delete plan summary is invalid");
    }
    for (const auto& target : summary.targets) {
        if (!valid_stable_value(target.recovery_point_id, kMaximumIdentifierBytes) ||
            target.catalog_generation == 0 || target.member_count == 0 ||
            !valid_wire_integer(target.catalog_generation) ||
            !valid_wire_integer(static_cast<std::uint64_t>(target.member_count))) {
            return invalid("delete plan target is invalid");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_execute_delete_plan_command(const ExecuteDeletePlanCommand& command) {
    if (!valid_token(command.plan_token) || !command.confirmed) {
        return invalid("execute delete plan command is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_protection_spec_input(const ProtectionSpecInput& protection,
                                                  const bool is_create) {
    if (!is_known_content_kind(protection.content_kind)) {
        return invalid("protection content_kind is invalid");
    }
    if (protection.content_kind == ContentKind::kVolumeSet) {
        if (!protection.file_selections.empty() ||
            !valid_source_ids(protection.volume_source_ids, !is_create)) {
            return invalid("volume protection sources are invalid");
        }
        if (is_create && protection.volume_source_ids.empty()) {
            return invalid("volume protection requires source ids on create");
        }
        return base::Result<void>::success();
    }
    if (!protection.volume_source_ids.empty()) {
        return invalid("file protection cannot include volume source ids");
    }
    if (protection.file_options.unreadable_policy != FileUnreadablePolicy::kFailJob) {
        return invalid("file protection options are invalid");
    }
    if (!is_create) {
        return protection.file_selections.empty()
                   ? base::Result<void>::success()
                   : invalid("file protection source is frozen on update");
    }
    if (protection.file_selections.empty() ||
        protection.file_selections.size() > kMaximumFileSelections) {
        return invalid("file protection selection count is invalid");
    }
    std::set<std::string_view> tokens;
    for (const auto& selection : protection.file_selections) {
        if (selection.node_token.empty() || selection.node_token.size() > kMaximumNodeTokenBytes ||
            !is_known_file_recursion(selection.recursion) ||
            !valid_text(selection.display_label, kMaximumDisplayLabelBytes) ||
            !tokens.insert(selection.node_token).second) {
            return invalid("file protection selection input is invalid");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_browse_file_sources_request(const BrowseFileSourcesRequest& request) {
    if (request.parent_node_token &&
        (request.parent_node_token->empty() ||
         request.parent_node_token->size() > kMaximumNodeTokenBytes)) {
        return invalid("browse parent_node_token is invalid");
    }
    return validate_service_page_request(request.page);
}

base::Result<void> validate_file_source_node(const FileSourceNode& node) {
    if (node.node_token.empty() || node.node_token.size() > kMaximumNodeTokenBytes ||
        !valid_text(node.display_name, kMaximumDisplayNameBytes) ||
        !is_known_file_entry_kind(node.entry_kind) ||
        (node.selectability != FileNodeSelectability::kSelectable &&
         node.selectability != FileNodeSelectability::kNotSelectable &&
         node.selectability != FileNodeSelectability::kUnsupported) ||
        (node.availability != SourceAvailability::kAvailable &&
         node.availability != SourceAvailability::kUnavailable) ||
        (node.message_code &&
         !valid_stable_value(*node.message_code, kMaximumMessageCodeBytes))) {
        return invalid("file source node is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_source_node_page(const FileSourceNodePage& page) {
    return validate_page(page, validate_file_source_node);
}

base::Result<void>
validate_list_recovery_point_entries_request(const ListRecoveryPointEntriesRequest& request) {
    if ((request.repository_connection_id &&
         !valid_stable_value(*request.repository_connection_id, kMaximumIdentifierBytes)) ||
        !valid_stable_value(request.recovery_point_id, kMaximumIdentifierBytes) ||
        request.parent_entry_id.empty() || request.parent_entry_id.size() > 20 ||
        (request.archive_secret_ref && request.archive_secret_ref->size() > 512)) {
        return invalid("list recovery point entries request is invalid");
    }
    return validate_service_page_request(request.page);
}

base::Result<void>
validate_recovery_point_entry_summary(const RecoveryPointEntrySummary& summary) {
    if (summary.entry_id.empty() || summary.entry_id.size() > 20 ||
        !valid_text(summary.display_name, kMaximumDisplayNameBytes) ||
        !is_known_file_entry_kind(summary.entry_kind) ||
        !valid_wire_integer(summary.logical_size_bytes) ||
        (summary.message_code &&
         !valid_stable_value(*summary.message_code, kMaximumMessageCodeBytes))) {
        return invalid("recovery point entry summary is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_recovery_point_entry_page(const RecoveryPointEntryPage& page) {
    if ((page.repository_connection_id &&
         !valid_stable_value(*page.repository_connection_id, kMaximumIdentifierBytes)) ||
        !valid_stable_value(page.recovery_point_id, kMaximumIdentifierBytes) ||
        page.parent_entry_id.empty() || page.index_generation.empty() ||
        page.index_generation.size() > 128 || page.items.size() > kMaximumServicePageResults ||
        !valid_token(page.continuation_token)) {
        return invalid("recovery point entry page is invalid");
    }
    for (const auto& item : page.items) {
        auto valid = validate_recovery_point_entry_summary(item);
        if (!valid) {
            return valid;
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_prepare_file_restore_request(const PrepareFileRestoreRequest& request) {
    if ((request.repository_connection_id &&
         !valid_stable_value(*request.repository_connection_id, kMaximumIdentifierBytes)) ||
        !valid_stable_value(request.recovery_point_id, kMaximumIdentifierBytes) ||
        request.entry_ids.empty() || request.entry_ids.size() > kMaximumFileRestoreEntryIds ||
        request.target_node_token.empty() ||
        request.target_node_token.size() > kMaximumNodeTokenBytes ||
        !is_known_file_conflict_policy(request.conflict_policy) || !request.restore_security ||
        (request.archive_secret_ref && request.archive_secret_ref->size() > 512)) {
        return invalid("prepare file restore request is invalid");
    }
    std::set<std::string_view> seen;
    for (const auto& entry_id : request.entry_ids) {
        if (entry_id.empty() || entry_id.size() > 20 || !seen.insert(entry_id).second) {
            return invalid("prepare file restore entry_ids are invalid");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_restore_preflight(const FileRestorePreflight& preflight) {
    if (!valid_token(preflight.preflight_token) ||
        (preflight.repository_connection_id &&
         !valid_stable_value(*preflight.repository_connection_id, kMaximumIdentifierBytes)) ||
        !valid_stable_value(preflight.recovery_point_id, kMaximumIdentifierBytes) ||
        preflight.entry_count == 0 || !valid_wire_integer(preflight.entry_count) ||
        !valid_wire_integer(preflight.logical_size_bytes) ||
        !valid_wire_integer(preflight.target_free_bytes) ||
        !is_known_file_conflict_policy(preflight.conflict_policy) ||
        preflight.expires_utc_ms == 0 || !valid_wire_integer(preflight.expires_utc_ms) ||
        !valid_stable_value(preflight.message_code, kMaximumMessageCodeBytes)) {
        return invalid("file restore preflight is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_start_file_restore_command(const StartFileRestoreCommand& command) {
    if (!valid_token(command.preflight_token) || !command.confirmed ||
        (command.archive_secret_ref && command.archive_secret_ref->size() > 512)) {
        return invalid("start file restore command is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_service_settings_query(const ServiceSettingsQuery&) {
    return base::Result<void>::success();
}

base::Result<void> validate_service_settings(const ServiceSettings& settings) {
    if (!is_valid_job_retention_months(settings.job_retention_months) ||
        !valid_wire_integer(settings.updated_utc_ms)) {
        return invalid("service settings are invalid");
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_update_service_settings_command(const UpdateServiceSettingsCommand& command) {
    if (!is_valid_job_retention_months(command.job_retention_months)) {
        return invalid("update service settings command is invalid");
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
