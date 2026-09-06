#pragma once

#include "aegra/base/result.h"
#include "aegra/contracts/boot_check.h"
#include "aegra/contracts/job.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kBootCheckJobSchemaVersion = 3;
inline constexpr std::size_t kMaximumBootCheckChainDepth = 128;

/// One BootCheck job for the single-task AegraBootCheck process. The host
/// derives everything else from the authenticated archive chain and trusted
/// Service settings; the request never carries hypervisor paths or plaintext credentials.
struct BootCheckJobRequest final {
    std::uint32_t schema_version{kBootCheckJobSchemaVersion};
    std::string job_id;
    std::string trace_id;
    /// Provider selected by the user when the owning schedule was saved. The
    /// host must not fall back to another provider.
    BootCheckHypervisor hypervisor{BootCheckHypervisor::kVirtualBox};
    std::uint32_t cpu_count{8};
    std::uint32_t memory_mib{4096};
    /// Base-first volume_set archive chain (Full root ... tip).
    std::vector<std::string> source_refs;
    /// One credential per layer; an empty SecretRef means an unencrypted layer.
    std::vector<SecretRef> credential_refs;
    /// Private, empty (or absent) local directory owned by this job. The host
    /// creates the VMDK presentation, VBox home, VM folder, and differencing
    /// medium inside it and deletes it during cleanup.
    std::string job_directory;
    /// 0 = no deadline.
    std::uint64_t deadline_utc_ms{0};
};

[[nodiscard]] base::Result<void>
validate_boot_check_job_request(const BootCheckJobRequest& request);

} // namespace aegra::contracts
