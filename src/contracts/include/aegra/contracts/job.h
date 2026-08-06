#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kJobSchemaVersion = 3;

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
    BackupType type{BackupType::kFull};
    std::string parent_source_ref;
    SecretRef parent_credential_ref;
    std::string file_uuid;
    std::string backup_set_uuid;
    std::int64_t created_utc_ms{0};
    /// When true (default), pagefile.sys / hiberfil.sys / swapfile.sys extents are zero-filled
    /// without reading (aligned with Desktop "Exclude pagefile / hiberfil / swapfile").
    bool exclude_page_and_hibernation_files{true};
    /// When true, archive metadata/payload use AEAD with credential_refs password.
    /// When false, archive is written unencrypted and password must be empty.
    bool encryption_enabled{false};
};

/// Optional restore-mode options.
/// - Omitted or `disk_restore=false`: volume → volume (target_ref = Volume GUID path).
/// - `disk_restore=true`: disk → disk (target_ref = `\\.\PhysicalDriveN`).
struct RestoreOptions final {
    /// When true, restore a disk image to `\\.\PhysicalDriveN` target_ref.
    /// source_refs must be the base-first Full→…→tip chain.
    bool disk_restore{false};
    /// Source `disk_number` from archive Manifest.disks[] (required when disk_restore).
    std::uint32_t source_disk_number{0};
    /// Source `volume_index` from archive Manifest.volumes[] (volume restore path).
    /// When restore options are omitted, Worker treats this as 0.
    std::uint32_t source_volume_index{0};
    /// After successful disk restore, clear OFFLINE/READ_ONLY (data-disk path; default true).
    bool bring_target_online{true};
    /// Keep MBR signature / GPT DiskId from the source (default true; recommended for bootable).
    bool preserve_disk_signature{true};
    /// When target is larger than source, grow last data partition + NTFS/ReFS (default true).
    bool auto_expand_last_partition{true};
};

struct JobRequest final {
    std::uint32_t schema_version{kJobSchemaVersion};
    std::string job_id;
    std::string tenant_id;
    JobOperation operation{JobOperation::kBackup};
    std::vector<std::string> source_refs;
    std::string target_ref;
    std::vector<SecretRef> credential_refs;
    std::optional<BackupOptions> backup;
    std::optional<RestoreOptions> restore;
    std::string trace_id;
    std::int64_t deadline_utc_ms{0};
};

[[nodiscard]] base::Result<void> validate_job_request(const JobRequest& request);

} // namespace aegra::contracts
