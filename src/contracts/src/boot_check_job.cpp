#include "aegra/contracts/boot_check_job.h"

#include <algorithm>
#include <cstdint>
#include <limits>

namespace aegra::contracts {
namespace {

constexpr std::size_t kMaximumJobIdBytes = 64;
constexpr std::size_t kMaximumTraceIdBytes = 128;
constexpr std::size_t kMaximumPathBytes = 4'096;

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

/// The job id becomes a VM name, a pipe name, and file-system entries, so it is
/// restricted to lowercase alphanumerics and '-'.
[[nodiscard]] bool valid_job_id(const std::string& job_id) noexcept {
    if (job_id.empty() || job_id.size() > kMaximumJobIdBytes) {
        return false;
    }
    return std::ranges::all_of(job_id, [](const unsigned char value) {
        return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '-';
    });
}

[[nodiscard]] bool valid_bounded_text(const std::string& value,
                                      const std::size_t maximum_bytes) noexcept {
    if (value.empty() || value.size() > maximum_bytes) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x20U && character != 0x7FU;
    });
}

} // namespace

base::Result<void> validate_boot_check_job_request(const BootCheckJobRequest& request) {
    if (request.schema_version != kBootCheckJobSchemaVersion) {
        return base::Result<void>::failure(
            {base::ErrorCode::kUnsupportedVersion, "unsupported boot check job schema version"});
    }
    if (!valid_job_id(request.job_id)) {
        return invalid("boot check job id is invalid");
    }
    if (!valid_bounded_text(request.trace_id, kMaximumTraceIdBytes)) {
        return invalid("boot check trace id is invalid");
    }
    if (!is_known_boot_check_hypervisor(request.hypervisor)) {
        return invalid("boot check hypervisor is invalid");
    }
    if (request.source_refs.empty() || request.source_refs.size() > kMaximumBootCheckChainDepth) {
        return invalid("boot check source chain is invalid");
    }
    for (const auto& source : request.source_refs) {
        if (!valid_bounded_text(source, kMaximumPathBytes)) {
            return invalid("boot check source reference is invalid");
        }
    }
    if (request.credential_refs.size() != request.source_refs.size()) {
        return invalid("boot check credentials must match the source chain");
    }
    for (const auto& credential : request.credential_refs) {
        if (credential.value.size() > kMaximumPathBytes) {
            return invalid("boot check credential reference is invalid");
        }
    }
    if (!valid_bounded_text(request.job_directory, kMaximumPathBytes)) {
        return invalid("boot check job directory is invalid");
    }
    if (request.deadline_utc_ms >
        static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return invalid("boot check deadline exceeds the wire range");
    }
    return base::Result<void>::success();
}

} // namespace aegra::contracts
