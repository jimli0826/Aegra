#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/process_launcher.h"
#include "aegra/ports/random.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace aegra::apps::boot_check {

/// Exit codes mirror the AegraWorker table so process supervision can share
/// the same classification.
enum class BootCheckExitCode : std::int32_t {
    kSucceeded = 0,
    kTaskFailed = 10,
    kCancelled = 11,
    kRequestRejected = 20,
    kHostFailure = 21,
};

/// Trusted host configuration. Nothing in this struct comes from the job
/// message; the composition root resolves it before any request is read.
struct BootCheckHostOptions final {
    /// Trusted absolute VBoxManage.exe path (empty = provider unavailable).
    std::string vbox_manage_path;
    /// Isolated VBOX_USER_HOME for the provider capability probe.
    std::string capability_user_home;
    /// Trusted absolute powershell.exe (System32); empty = Hyper-V unavailable.
    std::string powershell_path;
    /// True (default) isolates the VirtualBox job VM in a per-job registry
    /// (LocalSystem path). The composition root sets this false when the host is
    /// launched under the logged-on user's token, so the job VM registers in that
    /// user's default VirtualBox registry and is visible in their VirtualBox
    /// Manager. Does not affect the capability probe, which stays isolated.
    bool use_isolated_vbox_home{true};
    std::uint64_t overlay_limit_bytes{8ULL * 1024ULL * 1024ULL * 1024ULL};
    /// Overlay-growth FALLBACK threshold, used only when the hypervisor guest
    /// heartbeat is unavailable (Hyper-V Integration Services disabled, or
    /// VirtualBox without Guest Additions). Confirmation additionally requires a
    /// minimum elapsed time and sustained growth, so this alone does not pass an
    /// early-boot spinner. The primary signal is the guest heartbeat.
    std::uint64_t boot_confirmed_overlay_bytes{60ULL * 1024ULL * 1024ULL};
    std::uint64_t boot_timeout_ms{10ULL * 60ULL * 1000ULL};
    /// Extra time the guest keeps running after boot is confirmed, before the
    /// final screenshot. The heartbeat comes up early in boot (still on the
    /// Windows spinner), so without this the screenshot rarely shows the logon
    /// screen. The VM is still watched for power-off/quota/cancel meanwhile.
    std::uint64_t boot_settle_ms{90ULL * 1000ULL};
    /// Upper bound for a single archive chunk during chain random access.
    std::uint64_t maximum_chunk_bytes{256ULL * 1024ULL * 1024ULL};
    /// Byte budget of decoded archive chunks kept for the guest's random reads.
    std::uint64_t chunk_cache_budget_bytes{1024ULL * 1024ULL * 1024ULL};
};

struct BootCheckHostContext final {
    ports::ICredentialResolver& credentials;
    ports::IRandomSource& random;
    const ports::IClock& clock;
    ports::IProcessLauncher& launcher;
};

struct EncodedBootCheckResult final {
    BootCheckExitCode exit_code{BootCheckExitCode::kHostFailure};
    std::string response_json;
};

/// Executes one encoded BootCheck job. The response reuses the WorkerResponse
/// wire shape (schema 1) so the Service supervisor can share its decoder.
[[nodiscard]] base::Result<EncodedBootCheckResult>
run_boot_check_host_request(std::string_view encoded_request, const BootCheckHostOptions& options,
                            const BootCheckHostContext& context,
                            const base::CancellationToken& cancellation);

/// Startup scavenger (--scavenge): powers off and unregisters every orphaned
/// Aegra-BootCheck VM found in the isolated per-job registries under
/// <data_dir>/bootcheck/jobs, deletes those job directories, and clears stale
/// staged requests. Best-effort; kTaskFailed only when a directory survived.
[[nodiscard]] BootCheckExitCode
run_boot_check_scavenge(const BootCheckHostOptions& options, const BootCheckHostContext& context,
                        const std::filesystem::path& data_directory);

/// Capability probe mode (--inspect <virtualbox|hyperv>): runs the provider's
/// inspect() and prints one JSON line {schema_version, kind:"inspect",
/// hypervisor, available, message_code, provider_version, diagnostic}. An
/// unusable hypervisor still exits kSucceeded with available=false; only a
/// malformed argument is rejected.
[[nodiscard]] base::Result<EncodedBootCheckResult>
run_boot_check_inspect(std::string_view hypervisor_name, const BootCheckHostOptions& options,
                       const BootCheckHostContext& context);

/// Diagnostic mode (--present-only): presents the chain as a read-only VMDK
/// and holds the mount for hold_minutes so an external hypervisor can open it,
/// then cleans up. Never creates a VM; same response shape and exit codes.
[[nodiscard]] base::Result<EncodedBootCheckResult>
run_boot_check_present_request(std::string_view encoded_request,
                               const BootCheckHostOptions& options,
                               const BootCheckHostContext& context, std::uint32_t hold_minutes,
                               const base::CancellationToken& cancellation);

} // namespace aegra::apps::boot_check
