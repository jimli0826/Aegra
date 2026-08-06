#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/job.h"
#include "aegra/contracts/progress.h"
#include "aegra/contracts/repository_query.h"
#include "aegra/contracts/task_result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kMaximumServicePageResults = 100;
inline constexpr std::uint32_t kMaximumUnacknowledgedServiceEvents = 128;
inline constexpr std::uint32_t kMaximumBackupSources = 100;

struct MessageArgument final {
    std::string name;
    std::string value;
};

using MessageArguments = std::vector<MessageArgument>;

struct ServiceVersionRange final {
    std::uint32_t minimum_api_version{3};
    std::uint32_t maximum_api_version{3};
};

struct ServicePageRequest final {
    std::uint32_t maximum_results{50};
    std::optional<std::string> continuation_token;
};

struct ServiceRecoveryPointListRequest final {
    std::optional<std::string> repository_connection_id;
    RecoveryPointListRequest page;
};

struct ServiceRecoveryPointPage final {
    std::optional<std::string> repository_connection_id;
    RecoveryPointPage catalog;
};

enum class RepositoryConnectionState : std::uint8_t {
    kAvailable = 1,
    kUnavailable = 2,
};

struct RepositoryConnectionSummary final {
    std::string connection_id;
    std::string display_name;
    RepositoryConnectionState state{RepositoryConnectionState::kUnavailable};
    bool is_default{false};
    std::vector<std::string> capabilities;
};

struct RepositoryConnectionListRequest final {
    ServicePageRequest page;
    std::optional<RepositoryConnectionState> state;
};

enum class SourceKind : std::uint8_t {
    kVolume = 1,
};

enum class SourceAvailability : std::uint8_t {
    kAvailable = 1,
    kUnavailable = 2,
};

struct SourceInventoryItem final {
    std::string source_id;
    std::string display_name;
    SourceKind kind{SourceKind::kVolume};
    SourceAvailability availability{SourceAvailability::kUnavailable};
    std::uint64_t capacity_bytes{0};
    // Free space on the volume when known; 0 if unknown or unmounted.
    std::uint64_t free_bytes{0};
    // Physical disk total size when known; 0 means unavailable.
    std::uint64_t disk_capacity_bytes{0};
    bool is_system{false};
    bool is_read_only{false};
    bool is_selectable{false};
    // Physical disk index for Backup wizard disk/volume tree (old GUI disksTree).
    std::uint32_t disk_number{0};
    // Mount letter like "C:" or empty when none.
    std::string mount_letter;
    // Volume label / friendly name (may equal display_name).
    std::string volume_label;
    // Short health line for wizard, e.g. "Healthy" / "Healthy (Boot, System)".
    std::string health_status;
    // Disk partition style shown on the disk row, e.g. "GPT".
    std::string partition_style;
    // Physical media type for Home disk charts, e.g. "SSD".
    std::string media_type;
};

struct SourceInventoryListRequest final {
    ServicePageRequest page;
    bool include_unavailable{true};
};

enum class ServiceJobState : std::uint8_t {
    kQueued = 1,
    kRunning = 2,
    kCancelling = 3,
    kSucceeded = 4,
    kFailed = 5,
    kCancelled = 6,
    kInterrupted = 7,
};

struct JobSummary final {
    std::string job_id;
    std::string trace_id;
    JobOperation operation{JobOperation::kBackup};
    ServiceJobState state{ServiceJobState::kQueued};
    std::uint64_t created_utc_ms{0};
    std::optional<std::uint64_t> started_utc_ms;
    std::optional<std::uint64_t> completed_utc_ms;
    std::optional<TaskProgress> progress;
    std::string message_code;
    // Present for backup/restore jobs when the control plane stored them.
    std::vector<std::string> source_ids;
    std::optional<std::string> repository_connection_id;
};

struct JobListRequest final {
    ServicePageRequest page;
    std::optional<JobOperation> operation;
    std::optional<ServiceJobState> state;
};

enum class ScheduleTriggerKind : std::uint8_t {
    kDaily = 1,
    kWeekly = 2,
};

struct ScheduleTrigger final {
    ScheduleTriggerKind kind{ScheduleTriggerKind::kDaily};
    std::uint16_t local_minute_of_day{0};
    std::uint8_t weekday_mask{0};
    std::string timezone_id;
};

struct ScheduleSummary final {
    std::string schedule_id;
    std::string display_name;
    bool enabled{false};
    std::vector<std::string> source_ids;
    std::string repository_connection_id;
    BackupType backup_type{BackupType::kFull};
    ScheduleTrigger trigger;
    std::optional<std::uint64_t> next_run_utc_ms;
    bool exclude_page_and_hibernation_files{true};
    bool encryption_enabled{false};
};

struct ScheduleListRequest final {
    ServicePageRequest page;
    std::optional<bool> enabled;
};

enum class AuditSeverity : std::uint8_t {
    kInformation = 1,
    kWarning = 2,
    kError = 3,
    kCritical = 4,
};

struct AuditEventSummary final {
    std::string event_id;
    std::uint64_t created_utc_ms{0};
    AuditSeverity severity{AuditSeverity::kInformation};
    std::string message_code;
    MessageArguments message_arguments;
    std::string correlation_id;
};

struct AuditEventListRequest final {
    ServicePageRequest page;
    std::optional<AuditSeverity> minimum_severity;
    std::optional<std::uint64_t> from_utc_ms;
    std::optional<std::uint64_t> to_utc_ms;
    std::optional<std::string> correlation_id;
};

enum class MountSessionState : std::uint8_t {
    kMounting = 1,
    kMounted = 2,
    kUnmounting = 3,
    kFailed = 4,
};

struct MountSessionSummary final {
    std::string session_id;
    std::string recovery_point_id;
    MountSessionState state{MountSessionState::kMounting};
    std::string mount_point;
    std::uint64_t started_utc_ms{0};
    std::string message_code;
};

struct MountSessionListRequest final {
    ServicePageRequest page;
    std::optional<MountSessionState> state;
};

template <typename Item> struct ServicePage final {
    std::vector<Item> items;
    std::optional<std::string> continuation_token;
};

using RepositoryConnectionPage = ServicePage<RepositoryConnectionSummary>;
using SourceInventoryPage = ServicePage<SourceInventoryItem>;
using JobPage = ServicePage<JobSummary>;
using SchedulePage = ServicePage<ScheduleSummary>;
using AuditEventPage = ServicePage<AuditEventSummary>;
using MountSessionPage = ServicePage<MountSessionSummary>;

struct RepositoryConnectionInput final {
    std::string display_name;
    std::string locator;
    std::optional<SecretRef> credential_ref;
};

struct ResourceRef final {
    std::string resource_id;
};

// Trusted Service-side identity for a Recovery Point inside one Repository connection.
// Desktop never supplies Archive paths, object keys, or chain arrays.
struct RecoveryPointRef final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    /// Optional one-shot password to open an encrypted Archive (layout / inspect).
    /// Empty for unencrypted archives. Never logged.
    std::string archive_password;
};

struct StartBackupCommand final {
    /// Required. Sources, repository, exclude options, encryption and password ciphertext
    /// are loaded from the durable schedule record (not accepted on the wire).
    std::string schedule_id;
    /// Per-run type: Full or Incremental. Incremental parent is selected by Service.
    BackupType backup_type{BackupType::kFull};
};

struct StartVerifyCommand final {
    std::string repository_connection_id;
    std::string recovery_point_id;
};

enum class RecoveryPointStructuralState : std::uint8_t {
    kComplete = 1,
    kIncomplete = 2,
    kCorrupt = 3,
};

enum class RecoveryPointAuthenticationState : std::uint8_t {
    kNotAttempted = 1,
    kAuthenticated = 2,
    kFailed = 3,
    kCredentialRequired = 4,
};

enum class RecoveryPointChainCompleteness : std::uint8_t {
    kComplete = 1,
    kIncomplete = 2,
    kInvalid = 3,
};

struct RecoveryPointChainLayer final {
    std::string recovery_point_id;
    BackupType backup_type{BackupType::kFull};
    std::optional<std::string> parent_recovery_point_id;
    RecoveryPointStructuralState structural_state{RecoveryPointStructuralState::kComplete};
    RecoveryPointAuthenticationState authentication_state{
        RecoveryPointAuthenticationState::kNotAttempted};
    RecoveryPointChainCompleteness chain_state{RecoveryPointChainCompleteness::kIncomplete};
};

struct RecoveryPointChainResult final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    std::vector<RecoveryPointChainLayer> layers;
    bool restore_eligible{false};
    bool mount_eligible{false};
    bool verify_eligible{false};
    std::string message_code{"recovery_point.chain_ready"};
};

/// Partition entry from Manifest disks[] (for Restore Source Disks / reserved filtering).
struct RecoveryPointSourcePartition final {
    std::uint32_t partition_number{0};
    std::uint64_t offset_bytes{0};
    std::uint64_t size_bytes{0};
    bool is_active{false};
    std::uint8_t mbr_type{0};
    std::string gpt_type_guid;
    std::string gpt_name;
    std::string volume_label;
    std::string filesystem;
};

/// Physical disk from Manifest disks[] (old Restore layout primary axis).
struct RecoveryPointSourceDisk final {
    std::uint32_t disk_number{0};
    std::uint64_t disk_size_bytes{0};
    /// Stable code: "mbr" | "gpt" | "raw".
    std::string partition_style;
    std::string model;
    std::string media_type;
    std::vector<RecoveryPointSourcePartition> partitions;
};

/// Volume→partition join key (Manifest volumes[].extents[]).
struct RecoveryPointSourceExtent final {
    std::uint32_t disk_number{0};
    std::uint32_t partition_number{0};
    std::uint64_t physical_offset{0};
    std::uint64_t volume_offset{0};
    std::uint64_t length{0};
};

/// Source volume geometry from an archive Manifest (personal volume backup layout).
struct RecoveryPointSourceVolume final {
    std::uint32_t volume_index{0};
    std::string letter;
    std::string label;
    std::string filesystem;
    std::uint64_t total_size_bytes{0};
    std::vector<RecoveryPointSourceExtent> extents;
};

struct RecoveryPointLayout final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    std::vector<RecoveryPointSourceDisk> disks;
    std::vector<RecoveryPointSourceVolume> volumes;
};

struct DeletePlanTargetSummary final {
    std::string recovery_point_id;
    std::uint64_t catalog_generation{0};
    std::uint32_t member_count{0};
};

struct DeletePlanSummary final {
    std::string plan_token;
    std::string operation_id;
    std::string repository_connection_id;
    std::string root_recovery_point_id;
    std::vector<DeletePlanTargetSummary> targets;
    std::uint64_t expires_utc_ms{0};
};

struct ExecuteDeletePlanCommand final {
    std::string plan_token;
    bool confirmed{false};
};

struct RestorePreflightRequest final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    /// Inventory opaque id: volume source_id (`vol.…`) for volume restore, or `disk.N` for disk.
    std::string target_source_id;
    /// Manifest disk_number when restoring whole disk to a `disk.N` target.
    std::uint32_t source_disk_number{0};
    /// Manifest volume_index when restoring one volume to a `vol.…` target.
    std::uint32_t source_volume_index{0};
    /// Opens encrypted archives during preflight; empty for unencrypted. Never log.
    std::string archive_password;
};

struct RestorePreflight final {
    std::string preflight_token;
    std::string repository_connection_id;
    std::string recovery_point_id;
    std::string target_source_id;
    std::uint64_t logical_size_bytes{0};
    std::uint64_t target_capacity_bytes{0};
    std::uint32_t chain_depth{0};
    std::uint64_t expires_utc_ms{0};
    bool restore_eligible{false};
    std::string message_code;
};

struct StartRestoreCommand final {
    std::string preflight_token;
    bool confirmed{false};
    /// Same password used for preflight when the archive is encrypted; empty otherwise. Never log.
    std::string archive_password;
    /// Keep MBR signature / GPT DiskId (default true).
    bool preserve_disk_signature{true};
    /// Grow last data partition into free space when target is larger (default true).
    bool auto_expand_last_partition{true};
};

struct MountRecoveryPointCommand final {
    std::string recovery_point_id;
    std::optional<std::string> preferred_drive_letter;
};

struct UpsertScheduleCommand final {
    /// Absent = create; present = update an existing schedule.
    /// Update mutability (Service enforces against the durable record):
    /// - Immutable after create: source_ids, backup_type, exclude_page_and_hibernation_files,
    ///   encryption_enabled, archive password (DPAPI ciphertext in SQLite).
    /// - Mutable: display_name, enabled, repository_connection_id, trigger (schedule settings).
    /// - Backup options other than future shutdown-on-complete stay create-time only.
    std::optional<std::string> schedule_id;
    std::string display_name;
    bool enabled{false};
    std::vector<std::string> source_ids;
    std::string repository_connection_id;
    BackupType backup_type{BackupType::kFull};
    ScheduleTrigger trigger;
    bool exclude_page_and_hibernation_files{true};
    bool encryption_enabled{false};
    /// Create-only when encryption_enabled: DPAPI-protected (LOCAL_MACHINE,
    /// pOptionalEntropy = schedule_id) then base64; stored as
    /// schedules.archive_password_protected = dpapi-lm:<schedule_id>:<base64>.
    /// Must be empty on update (password cannot be set, cleared, or rotated after create).
    std::string archive_password;
};

struct EventSubscriptionRequest final {
    std::optional<std::string> resume_token;
    std::uint64_t after_sequence{0};
    std::uint32_t maximum_unacknowledged_events{64};
};

struct EventAcknowledgement final {
    std::string subscription_id;
    std::uint64_t through_sequence{0};
};

enum class CommandDisposition : std::uint8_t {
    kAccepted = 1,
    kReplayed = 2,
};

struct EventSubscriptionLease final {
    std::string subscription_id;
    std::string resume_token;
    std::uint64_t next_sequence{1};
    std::uint32_t maximum_unacknowledged_events{64};
};

struct CommandAcknowledgement final {
    std::string command_id;
    CommandDisposition disposition{CommandDisposition::kAccepted};
    std::optional<std::string> resource_id;
    std::optional<EventSubscriptionLease> event_subscription;
};

[[nodiscard]] base::Result<void> validate_message_arguments(const MessageArguments& arguments);
[[nodiscard]] base::Result<void> validate_service_version_range(const ServiceVersionRange& range);
[[nodiscard]] base::Result<void> validate_service_page_request(const ServicePageRequest& request);
[[nodiscard]] base::Result<void>
validate_service_recovery_point_list_request(const ServiceRecoveryPointListRequest& request);
[[nodiscard]] base::Result<void>
validate_service_recovery_point_page(const ServiceRecoveryPointPage& page);
[[nodiscard]] base::Result<void>
validate_repository_connection_list_request(const RepositoryConnectionListRequest& request);
[[nodiscard]] base::Result<void>
validate_repository_connection_summary(const RepositoryConnectionSummary& summary);
[[nodiscard]] base::Result<void>
validate_source_inventory_list_request(const SourceInventoryListRequest& request);
[[nodiscard]] base::Result<void> validate_source_inventory_item(const SourceInventoryItem& item);
[[nodiscard]] base::Result<void> validate_job_list_request(const JobListRequest& request);
[[nodiscard]] base::Result<void> validate_job_summary(const JobSummary& summary);
[[nodiscard]] base::Result<void> validate_schedule_trigger(const ScheduleTrigger& trigger);
[[nodiscard]] base::Result<void> validate_schedule_summary(const ScheduleSummary& summary);
[[nodiscard]] base::Result<void> validate_schedule_list_request(const ScheduleListRequest& request);
[[nodiscard]] base::Result<void> validate_audit_event_summary(const AuditEventSummary& summary);
[[nodiscard]] base::Result<void>
validate_audit_event_list_request(const AuditEventListRequest& request);
[[nodiscard]] base::Result<void> validate_mount_session_summary(const MountSessionSummary& summary);
[[nodiscard]] base::Result<void>
validate_mount_session_list_request(const MountSessionListRequest& request);
[[nodiscard]] base::Result<void>
validate_repository_connection_input(const RepositoryConnectionInput& input);
[[nodiscard]] base::Result<void> validate_resource_ref(const ResourceRef& reference);
[[nodiscard]] base::Result<void> validate_recovery_point_ref(const RecoveryPointRef& reference);
[[nodiscard]] base::Result<void> validate_start_backup_command(const StartBackupCommand& command);
[[nodiscard]] base::Result<void> validate_start_verify_command(const StartVerifyCommand& command);
[[nodiscard]] base::Result<void>
validate_restore_preflight_request(const RestorePreflightRequest& request);
[[nodiscard]] base::Result<void> validate_restore_preflight(const RestorePreflight& preflight);
[[nodiscard]] base::Result<void> validate_start_restore_command(const StartRestoreCommand& command);
[[nodiscard]] base::Result<void>
validate_mount_recovery_point_command(const MountRecoveryPointCommand& command);
[[nodiscard]] base::Result<void>
validate_upsert_schedule_command(const UpsertScheduleCommand& command);
[[nodiscard]] base::Result<void>
validate_event_subscription_request(const EventSubscriptionRequest& request);
[[nodiscard]] base::Result<void>
validate_event_acknowledgement(const EventAcknowledgement& acknowledgement);
[[nodiscard]] base::Result<void>
validate_command_acknowledgement(const CommandAcknowledgement& acknowledgement);
[[nodiscard]] base::Result<void>
validate_recovery_point_chain_result(const RecoveryPointChainResult& result);
[[nodiscard]] base::Result<void>
validate_recovery_point_layout(const RecoveryPointLayout& layout);
[[nodiscard]] base::Result<void> validate_delete_plan_summary(const DeletePlanSummary& summary);
[[nodiscard]] base::Result<void>
validate_execute_delete_plan_command(const ExecuteDeletePlanCommand& command);

[[nodiscard]] base::Result<void>
validate_repository_connection_page(const RepositoryConnectionPage& page);
[[nodiscard]] base::Result<void> validate_source_inventory_page(const SourceInventoryPage& page);
[[nodiscard]] base::Result<void> validate_job_page(const JobPage& page);
[[nodiscard]] base::Result<void> validate_schedule_page(const SchedulePage& page);
[[nodiscard]] base::Result<void> validate_audit_event_page(const AuditEventPage& page);
[[nodiscard]] base::Result<void> validate_mount_session_page(const MountSessionPage& page);

} // namespace aegra::contracts
