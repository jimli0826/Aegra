#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kJobSchemaVersion = 4;

enum class JobOperation : std::uint8_t {
    kBackup = 1,
    kRestore = 2,
    kVerify = 3,
    kExport = 4,
};

/// Opaque credential handle. Empty `value` means no password (unencrypted archive).
struct SecretRef final {
    std::string value;

    [[nodiscard]] friend bool operator==(const SecretRef&, const SecretRef&) = default;
};

enum class BackupType : std::uint8_t {
    kFull = 1,
    kIncremental = 2,
    kDifferential = 3,
};

struct BackupOptions final {
    /// Requested backup type (file_set may downgrade Incremental → Full).
    BackupType type{BackupType::kFull};
    std::string parent_source_ref;
    SecretRef parent_credential_ref;
    std::string file_uuid;
    std::string backup_set_uuid;
    std::int64_t created_utc_ms{0};
    /// When true (default), pagefile.sys / hiberfil.sys / swapfile.sys extents are zero-filled
    /// without reading (volume_set only; ignored for file_set).
    bool exclude_page_and_hibernation_files{true};
    /// volume_set: enable single-chunk DEDUP (ADR-0022); default true, frozen on schedule create.
    /// file_set: must be false.
    bool deduplication_enabled{true};
    /// When true, archive metadata/payload use AEAD with credential_refs password.
    /// When false, archive is written unencrypted and password must be empty.
    bool encryption_enabled{false};
    /// file_set only: Catalog tip UUID when Service selected an Incremental parent (empty = none).
    std::string candidate_parent_uuid;
    /// file_set only: absolute path to the parent .bkf for Worker open (empty when Full / demoted).
    /// volume_set: absolute path to parent archive (existing Incremental/Differential path).
    /// file_set reuses this field for parent Archive location (not volume geometry).
    /// file_set only: authenticated selection fingerprint for baseline / parent match.
    std::optional<FileSelectionFingerprint> selection_fingerprint;
    /// file_set only: Service already decided Full while `type` remains the *requested* type
    /// (must be Incremental). Worker writes Full and copies this reason into TaskResult.
    std::optional<IncrementalDowngradeReason> service_full_reason;
};

/// Volume/disk restore options (content_kind = volume_set).
struct RestoreOptions final {
    bool disk_restore{false};
    std::uint32_t source_disk_number{0};
    std::uint32_t source_volume_index{0};
    bool bring_target_online{true};
    bool preserve_disk_signature{true};
    bool auto_expand_last_partition{true};
};

/// Schema 4 Worker Job envelope.
/// Volume and file payloads are mutually exclusive based on `content_kind` and `operation`.
struct JobRequest final {
    std::uint32_t schema_version{kJobSchemaVersion};
    std::string job_id;
    std::string tenant_id;
    JobOperation operation{JobOperation::kBackup};
    ContentKind content_kind{ContentKind::kVolumeSet};

    /// volume_set: volume identities (backup) or archive path chain (restore/verify).
    /// file_set backup: must be empty.
    /// file_set restore/verify: archive path(s); restore uses tip or single Full archive.
    std::vector<std::string> source_refs;

    /// file_set backup only: trusted FileSourceRef list (1..100).
    std::vector<FileSourceRef> file_source_refs;

    /// volume_set backup: repository/archive destination.
    /// volume_set restore: volume GUID path or PhysicalDrive.
    /// file_set backup: repository/archive destination.
    /// verify: empty.
    /// file_set restore: empty (use file_restore_target).
    std::string target_ref;

    /// file_set restore only.
    std::optional<FileRestoreTarget> file_restore_target;

    std::vector<SecretRef> credential_refs;
    std::optional<BackupOptions> backup;
    std::optional<RestoreOptions> restore;
    std::string trace_id;
    std::int64_t deadline_utc_ms{0};
};

[[nodiscard]] base::Result<void> validate_job_request(const JobRequest& request);

} // namespace aegra::contracts
