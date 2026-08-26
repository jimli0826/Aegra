#include "aegra/application/pe_restore_prepare_service.h"

#include "aegra/ports/clock.h"

#include <span>
#include <utility>

namespace aegra::application {
namespace {

[[nodiscard]] base::Result<void> validate_request(const PeRestorePrepareRequest& request) {
    if (request.job_uuid.empty() || request.product_version.empty() ||
        request.data_dir_utf8.empty() || request.boot_entry_name.empty() ||
        request.executor_target_name.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "pe prepare request is missing required fields"});
    }
    if (request.payload.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "pe prepare request has no image payload"});
    }
    if (request.prompt_for_password && !request.archive_password.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "prompt mode must not carry a plaintext password"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] contracts::PePendingJobV1 assemble_job(const PeRestorePrepareRequest& request,
                                                     const std::int64_t created_utc_ms) {
    contracts::PePendingJobV1 job;
    job.job_uuid = request.job_uuid;
    job.created_utc_ms = created_utc_ms;
    job.product_version = request.product_version;
    job.source = request.source;
    job.target = request.target;
    job.options = request.options;
    job.ui = request.ui;
    return job;
}

/// Chooses the envelope mode and, for sealed mode, produces ciphertext + job key.
/// Sealing binds to the fully assembled job so any later field tampering fails to open.
[[nodiscard]] base::Result<IPeSecretSealer::Sealed>
seal_secret(IPeSecretSealer& sealer, const PeRestorePrepareRequest& request,
            const contracts::PePendingJobV1& job) {
    IPeSecretSealer::Sealed sealed;
    if (request.prompt_for_password) {
        sealed.envelope.mode = contracts::PeEnvelopeMode::kPrompt;
        return base::Result<IPeSecretSealer::Sealed>::success(std::move(sealed));
    }
    if (request.archive_password.empty()) {
        sealed.envelope.mode = contracts::PeEnvelopeMode::kNone;
        return base::Result<IPeSecretSealer::Sealed>::success(std::move(sealed));
    }
    return sealer.seal(request.archive_password, contracts::pe_pending_job_binding(job));
}

[[nodiscard]] ports::PeImageBuildRequest make_image_request(const PeRestorePrepareRequest& request) {
    ports::PeImageBuildRequest image;
    image.data_dir_utf8 = request.data_dir_utf8;
    image.product_version = request.product_version;
    image.payload = request.payload;
    image.executor_target_name = request.executor_target_name;
    image.debug_shell = request.debug_shell;
    return image;
}

[[nodiscard]] ports::OneTimeBootRequest make_boot_request(const PeRestorePrepareRequest& request,
                                                          const ports::PeImagePaths& image) {
    ports::OneTimeBootRequest boot;
    boot.wim_path_utf8 = image.wim_path_utf8;
    boot.sdi_path_utf8 = image.sdi_path_utf8;
    boot.entry_name = request.boot_entry_name;
    return boot;
}

} // namespace

PeRestorePrepareService::PeRestorePrepareService(ports::IPeImageBuilder& image_builder,
                                                 ports::IOneTimeBootController& boot_controller,
                                                 ports::IPePendingJobStore& pending_store,
                                                 IPeSecretSealer& sealer,
                                                 ports::IClock& clock) noexcept
    : image_builder_(image_builder), boot_controller_(boot_controller),
      pending_store_(pending_store), sealer_(sealer), clock_(clock) {}

base::Result<void>
PeRestorePrepareService::prepare_and_arm(const PeRestorePrepareRequest& request,
                                         const base::CancellationToken cancellation) {
    if (auto valid = validate_request(request); !valid) {
        return valid;
    }
    // Single occupancy: refuse while a prior hand-off is still pending (ADR-0026 B5).
    if (auto existing = pending_store_.read_pending(cancellation); existing) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "a pending pe restore already exists; cancel it first"});
    } else if (existing.error().code != base::ErrorCode::kNotFound) {
        return base::Result<void>::failure(existing.error());
    }
    auto image = image_builder_.ensure_ready(make_image_request(request), cancellation);
    if (!image) {
        return base::Result<void>::failure(image.error());
    }
    auto job = assemble_job(request, clock_.now_utc_ms());
    auto sealed = seal_secret(sealer_, request, job);
    if (!sealed) {
        return base::Result<void>::failure(sealed.error());
    }
    job.envelope = sealed.value().envelope;
    if (auto valid = contracts::validate_pe_pending_job(job); !valid) {
        return valid;
    }
    if (auto written = pending_store_.write_pending(
            job, std::span<const std::byte>(sealed.value().job_key), cancellation);
        !written) {
        return written;
    }
    return arm_or_rollback(request, image.value(), cancellation);
}

base::Result<void>
PeRestorePrepareService::arm_or_rollback(const PeRestorePrepareRequest& request,
                                         const ports::PeImagePaths& image,
                                         const base::CancellationToken cancellation) {
    auto armed = boot_controller_.arm_once(make_boot_request(request, image), cancellation);
    if (armed) {
        return base::Result<void>::success();
    }
    // Arm failed: the pending job (and its key) must not outlive the failed hand-off.
    (void)pending_store_.clear_pending();
    return armed;
}

base::Result<void> PeRestorePrepareService::cancel(const base::CancellationToken cancellation) {
    auto disarmed = boot_controller_.disarm(cancellation);
    auto cleared = pending_store_.clear_pending();
    if (!disarmed) {
        return disarmed;
    }
    return cleared;
}

base::Result<PeRestoreArmedState>
PeRestorePrepareService::query_state(const base::CancellationToken cancellation) {
    PeRestoreArmedState state;
    auto armed = boot_controller_.is_armed(cancellation);
    if (!armed) {
        return base::Result<PeRestoreArmedState>::failure(armed.error());
    }
    // "armed" here means "a pending hand-off exists to reboot into or to cancel",
    // not strictly that the one-time boot is still set. After PE boots, the boot
    // sequence is consumed but a job whose restore never completed still sits in
    // the store; the desktop must surface it so the user can cancel and re-arm.
    bool pending_present = false;
    auto pending = pending_store_.read_pending(cancellation);
    if (pending) {
        pending_present = true;
        state.job_uuid = pending.value().job_uuid;
        state.target_display = pending.value().target.friendly_name;
        state.created_utc_ms = pending.value().created_utc_ms;
    } else if (pending.error().code != base::ErrorCode::kNotFound) {
        return base::Result<PeRestoreArmedState>::failure(pending.error());
    }
    state.armed = armed.value() || pending_present;
    return base::Result<PeRestoreArmedState>::success(std::move(state));
}

} // namespace aegra::application
