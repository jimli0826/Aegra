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
    std::string trace_id;
    std::int64_t deadline_utc_ms{0};
};

[[nodiscard]] base::Result<void> validate_job_request(const JobRequest& request);

} // namespace aegra::contracts
