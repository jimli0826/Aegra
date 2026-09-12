#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/boot_check.h"
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
inline constexpr std::uint32_t kMaximumDeletePlanTargets = 10'000;

struct MessageArgument final {
    std::string name;
    std::string value;
};

using MessageArguments = std::vector<MessageArgument>;

struct ServiceVersionRange final {
    std::uint32_t minimum_api_version{4};
    std::uint32_t maximum_api_version{4};
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
    /// Stable repository root locator (local path / future network locator).
    std::string locator;
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
    kFileSelection = 2,
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
    // Primary extent start on the physical disk (bytes). 0 when unknown or disk shell.
    // Used by Desktop partition bars to place free space at the correct offset.
    std::uint64_t offset_bytes{0};
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
    std::optional<ContentKind> content_kind;
    std::uint64_t created_utc_ms{0};
    std::optional<std::uint64_t> started_utc_ms;
    std::optional<std::uint64_t> completed_utc_ms;
    std::optional<TaskProgress> progress;
    std::string message_code;
    // Present for backup/restore jobs when the control plane stored them.
    // Volume: inventory source ids. File: opaque selection ids (never paths).
    std::vector<std::string> source_ids;
    /// Backup jobs: owning schedule_id. Null/empty for restore, verify, and other ops.
    std::optional<std::string> schedule_id;
    std::optional<std::string> repository_connection_id;
    /// Requested backup type for backup jobs (control-plane insert); null otherwise.
    std::optional<std::uint8_t> requested_backup_type;
    /// file_set terminal result fields (from TaskResult); null until completed or non-file.
    std::optional<std::uint8_t> effective_backup_type;
    std::optional<std::string> effective_parent_uuid;
    std::optional<std::uint8_t> incremental_downgrade_reason;
};

/// Which job states ListJobs returns when `state` is null.
enum class JobListScope : std::uint8_t {
    kAll = 1,
    kActive = 2,   // queued, running, cancelling — operational hot path
    kTerminal = 3, // succeeded, failed, cancelled, interrupted — Task Log
};

struct JobListRequest final {
    ServicePageRequest page;
    std::optional<JobOperation> operation;
    /// When set, returns only this state (must be compatible with `scope`).
    std::optional<ServiceJobState> state;
    JobListScope scope{JobListScope::kAll};
    /// Inclusive lower bound on `created_utc_ms` (Task Log time filter).
    std::optional<std::uint64_t> from_utc_ms;
    /// Inclusive upper bound on `created_utc_ms`.
    std::optional<std::uint64_t> to_utc_ms;
};

enum class ScheduleTriggerKind : std::uint8_t {
    kDaily = 1,
    kWeekly = 2,
    kMonthly = 3,
};

/// Bit 0 = day 1 … bit 30 = day 31. Unused bit 31 must be 0.
inline constexpr std::uint32_t kMaximumDayOfMonthMask = 0x7FFFFFFFU;
/// Distinct local times of day per schedule (HH:MM slots as minutes since midnight).
inline constexpr std::size_t kMaximumLocalMinutesOfDay = 8;
/// Minimum gap between any two times on the same day (including wrap past midnight).
inline constexpr std::uint16_t kMinimumLocalMinutesOfDayGap = 30;

struct ScheduleTrigger final {
    ScheduleTriggerKind kind{ScheduleTriggerKind::kDaily};
    /// 1..kMaximumLocalMinutesOfDay values in [0, 1439], sorted unique. Empty until set.
    std::vector<std::uint16_t> local_minutes_of_day;
    std::uint8_t weekday_mask{0};
    /// Monthly only; 0 for Daily/Weekly.
    std::uint32_t day_of_month_mask{0};
    std::string timezone_id;
};

struct FileSelectionSummary final {
    std::string selection_id;
    std::string display_label;
    FileEntryKind entry_kind{FileEntryKind::kDirectory};
    FileRecursion recursion{FileRecursion::kRecursive};
    /// Volume-relative UI names (or [display_label] for a whole-volume selection). Not a path.
    std::vector<std::string> display_chain;
};

struct ScheduleSummary final {
    std::string schedule_id;
    std::string backup_set_uuid;
    std::string display_name;
    bool enabled{false};
    ContentKind content_kind{ContentKind::kVolumeSet};
    std::vector<std::string> source_ids;
    std::vector<FileSelectionSummary> selection_summaries;
    std::string repository_connection_id;
    BackupType backup_type{BackupType::kFull};
    ScheduleTrigger trigger;
    std::optional<std::uint64_t> next_run_utc_ms;
    bool exclude_page_and_hibernation_files{true};
    bool deduplication_enabled{true};
    std::uint64_t split_size_bytes{0};
    std::int32_t compression_level{kCompressionLevelNormal};
    /// When true, Service submits a separate Verify job after a successful Catalog publish.
    bool verify_after_backup{false};
    /// volume_set only; independent of verify_after_backup. BootCheck of the
    /// published recovery point in an isolated VM — after a successful Verify
    /// when verify is also enabled, otherwise directly after the backup.
    bool boot_check_after_backup{false};
    /// Present exactly when boot_check_after_backup is true.
    std::optional<BootCheckHypervisor> boot_check_hypervisor;
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
    std::uint32_t source_disk_number{0};
    std::uint64_t disk_size_bytes{0};
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
    /// Optional pre-protected SecretRef (rarely set by Desktop). Prefer network_* on create.
    std::optional<SecretRef> credential_ref;
    /// Create/import only for UNC network shares. Never log. Empty for local paths.
    /// Service packs these into credential_ref (DPAPI) before durable store.
    std::string network_username;
    std::string network_password;
    std::string network_domain;
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

/// Plan a chain-aware delete for one or more selected Recovery Point roots.
/// Service unions each root's descendant subtree into a single plan.
struct PlanDeleteRecoveryPointsRequest final {
    std::string repository_connection_id;
    std::vector<std::string> recovery_point_ids;
    /// Optional one-shot password; unused by catalog delete planning. Never logged.
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
    /// 1..kMaximumVerifyRecoveryPoints unique ids from one backup set. Service verifies
    /// earliest to latest in a single Worker process.
    std::vector<std::string> recovery_point_ids;
};

/// User-initiated boot check of one volume_set recovery point in an isolated VM.
/// Runs through the same durable plan/coordinator path as post-backup boot checks.
struct StartBootCheckCommand final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    /// Absent = the current service_settings default hypervisor.
    std::optional<BootCheckHypervisor> hypervisor;
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
    /// Free bytes at backup; equal to total_size_bytes when unknown (no used-space fill).
    std::uint64_t free_size_bytes{0};
    /// False when free was not recorded in the Manifest (skip used-space UI).
    bool free_size_known{false};
    std::vector<RecoveryPointSourceExtent> extents;
};

struct RecoveryPointLayout final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    /// "volume_set": disks/volumes populated. "file_set": no disk layout exists;
    /// the recovery point mounts as one read-only file namespace drive.
    std::string content_kind{"volume_set"};
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

/// Service quick filter vs Worker exact analysis outcome for volume restore.
enum class RestoreFeasibility : std::uint8_t {
    kIneligible = 1,
    kProvisional = 2,
    kEligible = 3,
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
    /// Disk targets must use kRequireSourceSize. Smaller NTFS volume analysis uses
    /// kAllowNtfsRelocation; capability `restore.ntfs_shrink.v1` remains gated separately.
    VolumeSizePolicy volume_size_policy{VolumeSizePolicy::kRequireSourceSize};
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
    VolumeSizePolicy volume_size_policy{VolumeSizePolicy::kRequireSourceSize};
    RestoreFeasibility feasibility{RestoreFeasibility::kIneligible};
    /// True only when feasibility == kEligible. StartRestore requires this.
    bool restore_eligible{false};
    /// Exact minimum target bytes when known; 0 when Service quick filter cannot compute it.
    std::uint64_t minimum_target_bytes{0};
    std::uint64_t relocation_bytes{0};
    std::uint64_t scratch_upper_bound_bytes{0};
    /// Empty until Worker exact ShrinkPlan exists.
    std::string shrink_plan_digest;
    /// Stable restriction codes (e.g. restore.shrink_below_minimum). No secrets.
    std::vector<std::string> restriction_codes;
    /// Stable warning codes. Warnings never replace failure.
    std::vector<std::string> warning_codes;
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
    /// Optional Target-bar layout (source start → target start + size). Empty = source geometry.
    std::vector<RestorePartitionLayoutEdit> partition_layout_edits;
};

/// kind 51 ArmPeRestore (WINPE_OFFLINE_RESTORE §4.1 / ADR-0026). `preflight_token`
/// comes from kind 19. `archive_password` (never log) seals into the cross-reboot
/// envelope; empty for unencrypted archives. `prompt_for_password` keeps secrets off
/// disk and lets the PE executor prompt. `locale` picks the PE UI language.
struct ArmPeRestoreCommand final {
    std::string preflight_token;
    bool confirmed{false};
    std::string archive_password;
    bool prompt_for_password{false};
    bool preserve_disk_signature{true};
    bool auto_expand_last_partition{false};
    std::string locale;
};

/// kind 20 GetPeRestoreState: parameterless request, armed-state response.
struct PeRestoreStateRequest final {};
struct PeRestoreState final {
    bool armed{false};
    std::string job_uuid;
    std::string target_display;
    std::int64_t created_utc_ms{0};
};

struct MountRecoveryPointCommand final {
    std::string repository_connection_id;
    std::string recovery_point_id;
    /// Manifest disk_number of the source disk to present.
    std::uint32_t source_disk_number{0};
    /// Single letter "D".."Z", or absent for automatic free letter selection.
    std::optional<std::string> preferred_drive_letter;
    /// Opens encrypted archives; empty when encryption is off. Never log.
    std::string archive_password;
};

struct FileSelectionInput final {
    std::string node_token;
    FileRecursion recursion{FileRecursion::kRecursive};
    std::string display_label;
};

struct FileSetOptionsInput final {
    FileUnreadablePolicy unreadable_policy{FileUnreadablePolicy::kFailJob};
};

/// Tagged protection object for UpsertSchedule (exact mutual exclusion).
struct ProtectionSpecInput final {
    ContentKind content_kind{ContentKind::kVolumeSet};
    /// volume_set create/list wire: 1..100 inventory source ids.
    std::vector<std::string> volume_source_ids;
    /// file_set create only: node tokens; empty on update (source frozen).
    std::vector<FileSelectionInput> file_selections;
    FileSetOptionsInput file_options{};
};

struct UpsertScheduleCommand final {
    /// Absent creates; present updates. Service freezes protection and backup options at create.
    std::optional<std::string> schedule_id;
    std::string display_name;
    bool enabled{false};
    ProtectionSpecInput protection{};
    std::string repository_connection_id;
    BackupType backup_type{BackupType::kFull};
    ScheduleTrigger trigger;
    bool exclude_page_and_hibernation_files{true};
    bool deduplication_enabled{true};
    std::uint64_t split_size_bytes{0};
    std::int32_t compression_level{kCompressionLevelNormal};
    bool verify_after_backup{false};
    /// Absent = keep the stored value on update / false on create. When set true:
    /// volume_set only and verify_after_backup must be true.
    std::optional<bool> boot_check_after_backup;
    /// Required when boot_check_after_backup=true; otherwise absent.
    std::optional<BootCheckHypervisor> boot_check_hypervisor;
    bool encryption_enabled{false};
    /// Create-only when encryption_enabled. Must be empty on update.
    std::string archive_password;
};

struct BrowseFileSourcesRequest final {
    std::optional<std::string> parent_node_token;
    ServicePageRequest page{};
    bool include_unavailable{false};
};

/// Lists immediate child directories of a previously connected Repository location.
/// location_token is issued by ConnectRepositoryLocation and is pipe-session bound.
struct RepositoryDirectoryListRequest final {
    std::string location_token;
    ServicePageRequest page{};
};

struct FileSourceNode final {
    std::string node_token;
    std::string display_name;
    FileEntryKind entry_kind{FileEntryKind::kDirectory};
    FileNodeSelectability selectability{FileNodeSelectability::kSelectable};
    bool has_children{false};
    bool is_directory{true};
    SourceAvailability availability{SourceAvailability::kAvailable};
    std::optional<std::string> message_code;
};

struct ListRecoveryPointEntriesRequest final {
    std::optional<std::string> repository_connection_id;
    std::string recovery_point_id;
    /// Decimal u64; root children use "0".
    std::string parent_entry_id{"0"};
    ServicePageRequest page{};
    std::optional<std::string> archive_secret_ref;
};

struct RecoveryPointEntrySummary final {
    std::string entry_id;
    std::string display_name;
    FileEntryKind entry_kind{FileEntryKind::kFile};
    std::uint64_t logical_size_bytes{0};
    bool has_children{false};
    std::optional<std::string> message_code;
};

struct RecoveryPointEntryPage final {
    std::optional<std::string> repository_connection_id;
    std::string recovery_point_id;
    std::string parent_entry_id;
    std::string index_generation;
    std::vector<RecoveryPointEntrySummary> items;
    std::optional<std::string> continuation_token;
};

struct PrepareFileRestoreRequest final {
    std::optional<std::string> repository_connection_id;
    std::string recovery_point_id;
    std::vector<std::string> entry_ids;
    std::string target_node_token;
    FileConflictPolicy conflict_policy{FileConflictPolicy::kFail};
    std::optional<std::string> archive_secret_ref;
    bool restore_security{true};
};

struct FileRestorePreflight final {
    std::string preflight_token;
    std::optional<std::string> repository_connection_id;
    std::string recovery_point_id;
    std::uint64_t entry_count{0};
    std::uint64_t logical_size_bytes{0};
    std::uint64_t target_free_bytes{0};
    FileConflictPolicy conflict_policy{FileConflictPolicy::kFail};
    std::uint64_t expires_utc_ms{0};
    bool restore_eligible{false};
    std::string message_code;
};

struct StartFileRestoreCommand final {
    std::string preflight_token;
    bool confirmed{false};
    std::optional<std::string> archive_secret_ref;
};

/// Allowed job history retention windows (calendar months approximated as 30 days).
inline constexpr std::uint8_t kDefaultJobRetentionMonths = 3;
inline constexpr std::uint32_t kMinimumBootCheckCpuCount = 1;
inline constexpr std::uint32_t kMaximumBootCheckCpuCount = 32;
inline constexpr std::uint32_t kDefaultBootCheckCpuCount = kMaximumBootCheckCpuCount;
inline constexpr std::uint32_t kMinimumBootCheckMemoryMib = 2U * 1024U;
inline constexpr std::uint32_t kMaximumBootCheckMemoryMib = 32U * 1024U;
inline constexpr std::uint32_t kDefaultBootCheckMemoryMib = 4U * 1024U;
inline constexpr std::uint32_t kMinimumBootCheckConcurrency = 1;
inline constexpr std::uint32_t kMaximumBootCheckConcurrency = 32;
inline constexpr std::uint32_t kDefaultBootCheckConcurrency = 2;
inline constexpr std::uint32_t kMinimumVerifyConcurrency = 1;
inline constexpr std::uint32_t kMaximumVerifyConcurrency = 32;
inline constexpr std::uint32_t kDefaultVerifyConcurrency = 2;

enum class VerifyScope : std::uint8_t {
    kSingleBackupFile = 1,
    kFullChain = 2,
};

[[nodiscard]] constexpr bool is_known_verify_scope(const VerifyScope scope) noexcept {
    return scope == VerifyScope::kSingleBackupFile || scope == VerifyScope::kFullChain;
}
inline constexpr std::uint64_t kMillisecondsPerRetentionMonth =
    30ULL * 24ULL * 60ULL * 60ULL * 1000ULL;

[[nodiscard]] constexpr bool is_valid_job_retention_months(const std::uint8_t months) noexcept {
    return months == 1 || months == 3 || months == 6;
}

/// Empty body for GetServiceSettings (exact_keys {}).
struct ServiceSettingsQuery final {};

/// Wire projection of control-plane service preferences.
struct ServiceSettings final {
    std::uint8_t job_retention_months{kDefaultJobRetentionMonths};
    BootCheckHypervisor default_boot_check_hypervisor{BootCheckHypervisor::kVirtualBox};
    std::uint32_t boot_check_cpu_count{kDefaultBootCheckCpuCount};
    std::uint32_t boot_check_memory_mib{kDefaultBootCheckMemoryMib};
    std::uint32_t boot_check_concurrency{kDefaultBootCheckConcurrency};
    std::uint32_t boot_check_effective_concurrency{kDefaultBootCheckConcurrency};
    VerifyScope verify_scope{VerifyScope::kSingleBackupFile};
    std::uint32_t verify_concurrency{kDefaultVerifyConcurrency};
    std::uint32_t host_logical_cpu_count{kDefaultBootCheckCpuCount};
    std::uint64_t host_physical_memory_mib{0};
    std::uint64_t boot_check_memory_budget_mib{0};
    std::uint64_t updated_utc_ms{0};
};

struct UpdateServiceSettingsCommand final {
    std::uint8_t job_retention_months{kDefaultJobRetentionMonths};
    BootCheckHypervisor default_boot_check_hypervisor{BootCheckHypervisor::kVirtualBox};
    std::uint32_t boot_check_cpu_count{kDefaultBootCheckCpuCount};
    std::uint32_t boot_check_memory_mib{kDefaultBootCheckMemoryMib};
    std::uint32_t boot_check_concurrency{kDefaultBootCheckConcurrency};
    VerifyScope verify_scope{VerifyScope::kSingleBackupFile};
    std::uint32_t verify_concurrency{kDefaultVerifyConcurrency};
};

/// Empty body for GetBootCheckHypervisorStatus (exact_keys {}).
struct BootCheckHypervisorStatusQuery final {};

/// Empty body for RefreshBootCheckHypervisorStatus (exact_keys {}).
struct RefreshBootCheckHypervisorStatusCommand final {};

/// Probe lifecycle of one boot-check hypervisor on this host.
enum class BootCheckProbeState : std::uint8_t {
    kNotProbed = 1,
    kProbing = 2,
    kProbed = 3,
};

[[nodiscard]] constexpr bool is_known_boot_check_probe_state(const BootCheckProbeState state) //
    noexcept {
    return state == BootCheckProbeState::kNotProbed || state == BootCheckProbeState::kProbing ||
           state == BootCheckProbeState::kProbed;
}

/// Usability of one hypervisor for boot check on this host. `available` and
/// `message_code` are meaningful only when probe_state is kProbed;
/// message_code carries the stable bootcheck.* reason when unavailable.
struct BootCheckHypervisorStatus final {
    BootCheckHypervisor hypervisor{BootCheckHypervisor::kVirtualBox};
    bool installed{false};
    BootCheckProbeState probe_state{BootCheckProbeState::kNotProbed};
    bool available{false};
    std::string message_code;
    std::uint64_t checked_utc_ms{0};
};

struct BootCheckHypervisorStatusReport final {
    std::vector<BootCheckHypervisorStatus> hypervisors;
};

using FileSourceNodePage = ServicePage<FileSourceNode>;

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
    /// Present after a successful Online TestRepositoryConnection free-space probe; null otherwise.
    std::optional<std::uint64_t> free_bytes;
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
[[nodiscard]] base::Result<void>
validate_plan_delete_recovery_points_request(const PlanDeleteRecoveryPointsRequest& request);
[[nodiscard]] base::Result<void> validate_start_backup_command(const StartBackupCommand& command);
[[nodiscard]] base::Result<void> validate_start_verify_command(const StartVerifyCommand& command);
[[nodiscard]] base::Result<void>
validate_start_boot_check_command(const StartBootCheckCommand& command);
[[nodiscard]] base::Result<void>
validate_restore_preflight_request(const RestorePreflightRequest& request);
[[nodiscard]] base::Result<void> validate_restore_preflight(const RestorePreflight& preflight);
[[nodiscard]] base::Result<void> validate_start_restore_command(const StartRestoreCommand& command);
[[nodiscard]] base::Result<void> validate_arm_pe_restore_command(const ArmPeRestoreCommand&);
[[nodiscard]] base::Result<void> validate_pe_restore_state_request(const PeRestoreStateRequest&);
[[nodiscard]] base::Result<void> validate_pe_restore_state(const PeRestoreState& state);
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
[[nodiscard]] base::Result<void> validate_recovery_point_layout(const RecoveryPointLayout& layout);
[[nodiscard]] base::Result<void> validate_delete_plan_summary(const DeletePlanSummary& summary);
[[nodiscard]] base::Result<void>
validate_execute_delete_plan_command(const ExecuteDeletePlanCommand& command);

[[nodiscard]] base::Result<void>
validate_protection_spec_input(const ProtectionSpecInput& protection, bool is_create);
[[nodiscard]] base::Result<void>
validate_browse_file_sources_request(const BrowseFileSourcesRequest& request);
[[nodiscard]] base::Result<void>
validate_repository_directory_list_request(const RepositoryDirectoryListRequest& request);
[[nodiscard]] base::Result<void> validate_file_source_node(const FileSourceNode& node);
[[nodiscard]] base::Result<void> validate_file_source_node_page(const FileSourceNodePage& page);
[[nodiscard]] base::Result<void>
validate_list_recovery_point_entries_request(const ListRecoveryPointEntriesRequest& request);
[[nodiscard]] base::Result<void>
validate_recovery_point_entry_summary(const RecoveryPointEntrySummary& summary);
[[nodiscard]] base::Result<void>
validate_recovery_point_entry_page(const RecoveryPointEntryPage& page);
[[nodiscard]] base::Result<void>
validate_prepare_file_restore_request(const PrepareFileRestoreRequest& request);
[[nodiscard]] base::Result<void>
validate_file_restore_preflight(const FileRestorePreflight& preflight);
[[nodiscard]] base::Result<void>
validate_start_file_restore_command(const StartFileRestoreCommand& command);
[[nodiscard]] base::Result<void> validate_service_settings_query(const ServiceSettingsQuery& query);
[[nodiscard]] base::Result<void> validate_service_settings(const ServiceSettings& settings);
[[nodiscard]] base::Result<void>
validate_update_service_settings_command(const UpdateServiceSettingsCommand& command);
[[nodiscard]] base::Result<void>
validate_boot_check_hypervisor_status_query(const BootCheckHypervisorStatusQuery& query);
[[nodiscard]] base::Result<void> validate_refresh_boot_check_hypervisor_status_command(
    const RefreshBootCheckHypervisorStatusCommand& command);
[[nodiscard]] base::Result<void>
validate_boot_check_hypervisor_status_report(const BootCheckHypervisorStatusReport& report);

[[nodiscard]] base::Result<void>
validate_repository_connection_page(const RepositoryConnectionPage& page);
[[nodiscard]] base::Result<void> validate_source_inventory_page(const SourceInventoryPage& page);
[[nodiscard]] base::Result<void> validate_job_page(const JobPage& page);
[[nodiscard]] base::Result<void> validate_schedule_page(const SchedulePage& page);
[[nodiscard]] base::Result<void> validate_audit_event_page(const AuditEventPage& page);
[[nodiscard]] base::Result<void> validate_mount_session_page(const MountSessionPage& page);

} // namespace aegra::contracts
