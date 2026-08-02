#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kJobSchemaVersion = 1;

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

struct JobRequest final {
    std::uint32_t schema_version{kJobSchemaVersion};
    std::string job_id;
    std::string tenant_id;
    JobOperation operation{JobOperation::kBackup};
    std::vector<std::string> source_refs;
    std::string target_ref;
    std::vector<SecretRef> credential_refs;
    std::string trace_id;
    std::int64_t deadline_utc_ms{0};
};

[[nodiscard]] base::Result<void> validate_job_request(const JobRequest& request);

} // namespace aegra::contracts
