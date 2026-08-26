#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/pe_restore.h"
#include "aegra/ports/one_time_boot.h"
#include "aegra/ports/pe_image_builder.h"
#include "aegra/ports/pe_pending_store.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::ports {
class IClock;
} // namespace aegra::ports

namespace aegra::application {

/// Seals a plaintext password into a cross-reboot envelope (ADR-0026 B). Injected
/// so the application layer never links the crypto adapter; the composition root
/// implements it over crypto_sodium::pe_envelope.
class IPeSecretSealer {
  public:
    IPeSecretSealer() = default;
    virtual ~IPeSecretSealer() = default;
    IPeSecretSealer(const IPeSecretSealer&) = delete;
    IPeSecretSealer& operator=(const IPeSecretSealer&) = delete;
    IPeSecretSealer(IPeSecretSealer&&) = delete;
    IPeSecretSealer& operator=(IPeSecretSealer&&) = delete;

    struct Sealed final {
        contracts::PeSecretEnvelope envelope;
        /// 32-byte job key to persist alongside the job (empty for none/prompt).
        std::vector<std::byte> job_key;
    };

    /// `binding` is contracts::pe_pending_job_binding(job) computed on the assembled job.
    [[nodiscard]] virtual base::Result<Sealed> seal(std::string_view password,
                                                    std::string_view binding) = 0;
};

/// Everything needed to build one PE restore hand-off. The chain, target identity,
/// and geometry are resolved and authenticated online before this call.
struct PeRestorePrepareRequest final {
    std::string job_uuid;
    std::string product_version;
    /// Absolute data directory (image + pending live beneath it).
    std::string data_dir_utf8;
    contracts::PeRestoreSource source;
    contracts::PeTargetDiskIdentity target;
    contracts::PeRestoreOptions options;
    contracts::PeRestoreUi ui;
    /// Payload closure for the WinPE image (executor + worker + runtime).
    std::vector<ports::PeImagePayloadFile> payload;
    std::string executor_target_name;
    /// Empty when the archive is unencrypted. For prompt mode, leave empty and set
    /// `prompt_for_password`.
    std::string archive_password;
    bool prompt_for_password{false};
    /// Boot entry menu name (fixed product constant supplied by the caller).
    std::string boot_entry_name;
    /// Diagnostics: build the PE image with an interactive cmd shell instead of
    /// auto-launching the executor (see PeImageBuildRequest::debug_shell).
    bool debug_shell{false};
};

/// Read-only view of the armed state for the desktop status bar (kind 20).
struct PeRestoreArmedState final {
    bool armed{false};
    std::string job_uuid;
    std::string target_display;
    std::int64_t created_utc_ms{0};
};

/// Online orchestration for WinPE system-disk restore (design §4.1): build/cache
/// the WinRE image, seal the password, write the pending job, and arm the one-time
/// boot. Every step rolls back on failure so a partial hand-off never survives.
class PeRestorePrepareService final {
  public:
    PeRestorePrepareService(ports::IPeImageBuilder& image_builder,
                            ports::IOneTimeBootController& boot_controller,
                            ports::IPePendingJobStore& pending_store, IPeSecretSealer& sealer,
                            ports::IClock& clock) noexcept;

    /// Full prepare + arm. On success the caller prompts the user to reboot.
    /// Refuses (kConflict) when a pending job already exists.
    [[nodiscard]] base::Result<void> prepare_and_arm(const PeRestorePrepareRequest& request,
                                                     base::CancellationToken cancellation);

    /// Cancel a pending hand-off before reboot: disarm then clear pending. Idempotent.
    [[nodiscard]] base::Result<void> cancel(base::CancellationToken cancellation);

    [[nodiscard]] base::Result<PeRestoreArmedState> query_state(base::CancellationToken cancellation);

  private:
    [[nodiscard]] base::Result<void> arm_or_rollback(const PeRestorePrepareRequest& request,
                                                     const ports::PeImagePaths& image,
                                                     base::CancellationToken cancellation);

    ports::IPeImageBuilder& image_builder_;
    ports::IOneTimeBootController& boot_controller_;
    ports::IPePendingJobStore& pending_store_;
    IPeSecretSealer& sealer_;
    ports::IClock& clock_;
};

} // namespace aegra::application
