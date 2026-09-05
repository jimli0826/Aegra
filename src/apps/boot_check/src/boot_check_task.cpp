#include "boot_check_task.h"

#include "aegra/adapters/dokan/vmdk_presentation.h"
#include "aegra/adapters/hyperv/hyperv_boot_check_provider.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/virtualbox/virtualbox_boot_check_provider.h"
#include "aegra/apps/worker/worker_task_log.h"
#include "aegra/contracts/boot_check.h"
#include "aegra/format/manifest.h"
#include "aegra/ports/credential.h"
#include "aegra/virtualization/boot_check_provider.h"

#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::apps::boot_check::detail {
namespace {

using apps::worker::ScopedStage;
using apps::worker::WorkerTaskLog;
using apps::worker::WorkerTaskLogScope;

constexpr const char* kCompleted = "bootcheck.completed";
constexpr const char* kSourceNotSystemDisk = "bootcheck.source_not_system_disk";
constexpr const char* kUnsupportedBootProfile = "bootcheck.unsupported_boot_profile";
constexpr const char* kProviderUnavailable = "bootcheck.provider_unavailable";
constexpr const char* kArchiveOpenFailed = "bootcheck.archive_open_failed";
constexpr const char* kVmdkPresentFailed = "bootcheck.vmdk_present_failed";
constexpr const char* kVmCreateFailed = "bootcheck.vm_create_failed";
constexpr const char* kVmStartFailed = "bootcheck.vm_start_failed";
constexpr const char* kGuestPoweredOff = "bootcheck.guest_powered_off";
constexpr const char* kBootNotConfirmed = "bootcheck.boot_not_confirmed";
constexpr const char* kOverlayFull = "bootcheck.overlay_full";
constexpr const char* kCancelled = "bootcheck.cancelled";
constexpr const char* kCleanupIncomplete = "bootcheck.cleanup_incomplete";
constexpr const char* kPresentHoldCompleted = "bootcheck.present_hold_completed";

constexpr auto kOverlayPollInterval = std::chrono::seconds(2);
constexpr auto kVmStatePollInterval = std::chrono::seconds(10);
constexpr auto kCleanupBudget = std::chrono::minutes(2);

[[nodiscard]] base::Error stage_error(const base::ErrorCode code, const char* message_code) {
    return base::Error{code, message_code};
}

[[nodiscard]] std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override { return {}; }
};

/// Everything a job materializes, torn down in reverse dependency order:
/// session (hypervisor media) -> provider -> presentation (Dokan) -> disk -> chain.
struct TaskResources final {
    std::unique_ptr<adapters::personal_archive::PersonalArchiveChainReader> chain;
    std::unique_ptr<adapters::personal_archive::WholeDiskByteReader> disk;
    std::unique_ptr<adapters::dokan::ReadOnlyVmdkPresentation> presentation;
    std::unique_ptr<adapters::dokan::ReadOnlyVhdxPresentation> vhdx_presentation;
    std::unique_ptr<virtualization::IBootCheckProvider> provider;
    std::string parent_disk_path;
    std::unique_ptr<virtualization::IBootCheckVmSession> session;
    virtualization::BootFirmware firmware{virtualization::BootFirmware::kBios};
    std::filesystem::path job_directory;
    bool owns_job_directory{false};
};

/// Requests stop after a fixed budget so cleanup can never hang the host.
class CleanupDeadline final {
  public:
    CleanupDeadline()
        : watchdog_([this](const std::stop_token stopped) {
              std::unique_lock lock(mutex_);
              changed_.wait_for(lock, stopped, kCleanupBudget, [] { return false; });
              if (!stopped.stop_requested()) {
                  cancellation_.request_stop();
              }
          }) {}

    ~CleanupDeadline() {
        watchdog_.request_stop();
        changed_.notify_all();
    }

    CleanupDeadline(const CleanupDeadline&) = delete;
    CleanupDeadline& operator=(const CleanupDeadline&) = delete;

    [[nodiscard]] base::CancellationToken token() const noexcept {
        return cancellation_.get_token();
    }

  private:
    base::CancellationSource cancellation_;
    std::mutex mutex_;
    std::condition_variable_any changed_;
    std::jthread watchdog_;
};


[[nodiscard]] base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>
resolve_secrets(const contracts::BootCheckJobRequest& request,
                ports::ICredentialResolver& credentials,
                const base::CancellationToken& cancellation) {
    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    secrets.reserve(request.credential_refs.size());
    for (const auto& reference : request.credential_refs) {
        // Empty SecretRef = unencrypted layer (empty password), matching restore.
        if (reference.value.empty()) {
            secrets.push_back(std::make_unique<EmptyPasswordSecret>());
            continue;
        }
        auto secret = credentials.resolve(reference, cancellation);
        if (!secret || secret.value() == nullptr) {
            const auto code = !secret && secret.error().code == base::ErrorCode::kCancelled
                                  ? base::ErrorCode::kCancelled
                                  : base::ErrorCode::kUnauthorized;
            return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::failure(
                stage_error(code, kArchiveOpenFailed));
        }
        secrets.push_back(std::move(secret).value());
    }
    return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::success(
        std::move(secrets));
}

[[nodiscard]] base::Result<void> prepare_job_directory(TaskResources& resources,
                                                       const std::string& job_directory) {
    resources.job_directory = path_from_utf8(job_directory);
    if (!resources.job_directory.is_absolute()) {
        return base::Result<void>::failure(
            stage_error(base::ErrorCode::kInvalidArgument, kVmCreateFailed));
    }
    std::error_code error;
    std::filesystem::create_directories(resources.job_directory, error);
    if (error || !std::filesystem::is_directory(resources.job_directory, error) || error) {
        return base::Result<void>::failure(
            stage_error(base::ErrorCode::kIoFailure, kVmCreateFailed));
    }
    // The host owns everything inside the job directory, so it must start empty;
    // cleanup later deletes the exact directory it validated here.
    if (!std::filesystem::is_empty(resources.job_directory, error) || error) {
        return base::Result<void>::failure(
            stage_error(base::ErrorCode::kConflict, kVmCreateFailed));
    }
    resources.owns_job_directory = true;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
open_archive_chain(const contracts::BootCheckJobRequest& request,
                   const BootCheckHostOptions& options,
                   const std::vector<std::unique_ptr<ports::IResolvedSecret>>& secrets,
                   TaskResources& resources, ScopedStage& stage) {
    adapters::personal_archive::ArchiveChainOpenRequest open_request;
    open_request.maximum_chain_depth =
        static_cast<std::uint32_t>(contracts::kMaximumBootCheckChainDepth);
    open_request.layers.reserve(request.source_refs.size());
    for (std::size_t index = 0; index < request.source_refs.size(); ++index) {
        adapters::personal_archive::ArchiveOpenRequest layer;
        layer.source = path_from_utf8(request.source_refs[index]);
        layer.password = secrets[index]->view();
        layer.maximum_chunk_payload_size = options.maximum_chunk_bytes;
        layer.maximum_chunk_logical_size = options.maximum_chunk_bytes;
        open_request.layers.push_back(std::move(layer));
    }
    auto chain = adapters::personal_archive::PersonalArchiveChainReader::open(open_request);
    if (!chain) {
        stage.fail(chain.error(), "PersonalArchiveChainReader::open");
        return base::Result<void>::failure(stage_error(chain.error().code, kArchiveOpenFailed));
    }
    resources.chain = std::move(chain).value();
    stage.note_u64("layers", request.source_refs.size());
    stage.note_bytes("logical_size", resources.chain->logical_size_bytes());
    stage.note_u64("chunk_count", resources.chain->chunk_count());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_boot_profile(TaskResources& resources,
                                                       const BootCheckHostOptions& options,
                                                       ScopedStage& stage) {
    const auto& manifest = resources.chain->manifest();
    if (manifest.content_kind != format::kManifestContentKindVolumeSet || !manifest.boot_profile) {
        const base::Error error = stage_error(base::ErrorCode::kConflict, kSourceNotSystemDisk);
        stage.fail(error, "boot_profile");
        return base::Result<void>::failure(error);
    }
    const auto& profile = *manifest.boot_profile;
    stage.note("firmware",
               profile.firmware_mode == format::BootFirmwareMode::kUefi ? "uefi" : "bios");
    stage.note_u64("system_disk", profile.system_disk_number);
    stage.note_u64("sector_size", profile.logical_sector_size);
    stage.note("os_build", profile.os_build);
    if (profile.os_architecture != format::BootOsArchitecture::kX64 ||
        profile.logical_sector_size != 512 ||
        profile.bitlocker_state == format::BootSecurityState::kEnabled) {
        const base::Error error = stage_error(base::ErrorCode::kConflict, kUnsupportedBootProfile);
        stage.fail(error, "boot_profile");
        return base::Result<void>::failure(error);
    }
    resources.firmware = profile.firmware_mode == format::BootFirmwareMode::kUefi
                             ? virtualization::BootFirmware::kUefi
                             : virtualization::BootFirmware::kBios;
    auto disk = adapters::personal_archive::WholeDiskByteReader::open(
        *resources.chain, manifest, profile.system_disk_number, options.chunk_cache_entries);
    if (!disk) {
        stage.fail(disk.error(), "WholeDiskByteReader::open");
        return base::Result<void>::failure(stage_error(disk.error().code, kSourceNotSystemDisk));
    }
    resources.disk = std::move(disk).value();
    stage.note_bytes("disk_size", resources.disk->size_bytes());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<adapters::dokan::VmdkImageIdentity>
make_image_identity(ports::IRandomSource& random, const base::CancellationToken& cancellation) {
    std::array<std::byte, 36> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<adapters::dokan::VmdkImageIdentity>::failure(
            stage_error(filled.error().code, kVmdkPresentFailed));
    }
    adapters::dokan::VmdkImageIdentity identity;
    std::memcpy(&identity.cid, bytes.data(), sizeof(identity.cid));
    identity.cid &= 0x7FFFFFFEU;
    identity.cid |= 0x2U;
    std::memcpy(identity.image_uuid.data(), bytes.data() + 4, identity.image_uuid.size());
    identity.image_uuid[0] |= 0x1U;
    std::memcpy(identity.modification_uuid.data(), bytes.data() + 20,
                identity.modification_uuid.size());
    identity.modification_uuid[0] |= 0x1U;
    return base::Result<adapters::dokan::VmdkImageIdentity>::success(identity);
}

/// Instantiates exactly the provider selected by the owning schedule. A failed
/// inspection is terminal; silently switching hypervisors would violate the
/// durable post-backup plan.
[[nodiscard]] base::Result<void> select_provider(const contracts::BootCheckHypervisor hypervisor,
                                                 const BootCheckHostOptions& options,
                                                 const BootCheckHostContext& context,
                                                 TaskResources& resources,
                                                 const base::CancellationToken& cancellation,
                                                 ScopedStage& stage) {
    // Providers may refine the generic code into an actionable one (for
    // example a VirtualBox/Hyper-V conflict) that the desktop can translate.
    std::string unavailable_code = kProviderUnavailable;
    if (hypervisor == contracts::BootCheckHypervisor::kVirtualBox) {
        stage.note("hypervisor", "virtualbox");
        if (options.vbox_manage_path.empty()) {
            stage.note("diagnostic",
                       "VBoxManage.exe not discovered (registry InstallDir or Program Files)");
        } else {
            stage.note("vbox_manage_path", options.vbox_manage_path);
            adapters::virtualbox::VirtualBoxProviderOptions provider_options;
            provider_options.vbox_manage_path = options.vbox_manage_path;
            provider_options.capability_user_home = options.capability_user_home;
            provider_options.use_isolated_home = options.use_isolated_vbox_home;
            auto provider = std::make_unique<adapters::virtualbox::VirtualBoxBootCheckProvider>(
                context.launcher, std::move(provider_options));
            auto inspected = provider->inspect(cancellation);
            if (!inspected) {
                stage.fail(inspected.error(), "VirtualBoxBootCheckProvider::inspect");
                return base::Result<void>::failure(inspected.error());
            }
            if (inspected.value().available) {
                stage.note("provider", inspected.value().provider_name);
                stage.note("provider_version", inspected.value().provider_version);
                resources.provider = std::move(provider);
                return base::Result<void>::success();
            }
            stage.note("diagnostic", inspected.value().diagnostic);
            if (!inspected.value().message_code.empty()) {
                unavailable_code = inspected.value().message_code;
            }
        }
    }
    if (hypervisor == contracts::BootCheckHypervisor::kHyperV) {
        stage.note("hypervisor", "hyperv");
        if (options.powershell_path.empty()) {
            stage.note("diagnostic", "powershell.exe not discovered");
        } else {
            stage.note("powershell_path", options.powershell_path);
            adapters::hyperv::HyperVProviderOptions provider_options;
            provider_options.powershell_path = options.powershell_path;
            auto provider = std::make_unique<adapters::hyperv::HyperVBootCheckProvider>(
                context.launcher, std::move(provider_options));
            auto inspected = provider->inspect(cancellation);
            if (!inspected) {
                stage.fail(inspected.error(), "HyperVBootCheckProvider::inspect");
                return base::Result<void>::failure(inspected.error());
            }
            if (inspected.value().available) {
                stage.note("provider", inspected.value().provider_name);
                stage.note("provider_version", inspected.value().provider_version);
                resources.provider = std::move(provider);
                return base::Result<void>::success();
            }
            stage.note("diagnostic", inspected.value().diagnostic);
            if (!inspected.value().message_code.empty()) {
                unavailable_code = inspected.value().message_code;
            }
        }
    }
    const base::Error error{base::ErrorCode::kNotFound, unavailable_code};
    stage.fail(error, "select_provider");
    return base::Result<void>::failure(error);
}

[[nodiscard]] base::Result<void> present_disk(TaskResources& resources,
                                              const BootCheckHostContext& context,
                                              const base::CancellationToken& cancellation,
                                              ScopedStage& stage) {
    const auto present_directory = resources.job_directory / "present";
    if (resources.provider->parent_disk_format() ==
        virtualization::BootCheckParentDiskFormat::kVhdx) {
        auto presentation = adapters::dokan::ReadOnlyVhdxPresentation::create(
            *resources.disk, present_directory, cancellation);
        if (!presentation) {
            stage.fail(presentation.error(), "ReadOnlyVhdxPresentation::create");
            return base::Result<void>::failure(
                stage_error(presentation.error().code, kVmdkPresentFailed));
        }
        resources.vhdx_presentation = std::move(presentation).value();
        resources.parent_disk_path =
            apps::worker::path_display(resources.vhdx_presentation->info().vhdx_path);
        stage.note("parent_disk", resources.parent_disk_path);
        stage.note_bytes("container_size", resources.vhdx_presentation->info().virtual_size_bytes);
        return base::Result<void>::success();
    }
    auto identity = make_image_identity(context.random, cancellation);
    if (!identity) {
        stage.fail(identity.error(), "make_image_identity");
        return base::Result<void>::failure(identity.error());
    }
    auto presentation = adapters::dokan::ReadOnlyVmdkPresentation::create(
        *resources.disk, present_directory, identity.value(), cancellation);
    if (!presentation) {
        stage.fail(presentation.error(), "ReadOnlyVmdkPresentation::create");
        return base::Result<void>::failure(
            stage_error(presentation.error().code, kVmdkPresentFailed));
    }
    resources.presentation = std::move(presentation).value();
    resources.parent_disk_path =
        apps::worker::path_display(resources.presentation->info().descriptor_path);
    stage.note("parent_disk", resources.parent_disk_path);
    stage.note_bytes("virtual_size", resources.presentation->info().virtual_size_bytes);
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> create_vm(const contracts::BootCheckJobRequest& request,
                                           const BootCheckHostOptions& options,
                                           TaskResources& resources,
                                           const base::CancellationToken& cancellation,
                                           ScopedStage& stage) {
    virtualization::BootCheckVmRequest vm_request;
    vm_request.job_id = request.job_id;
    vm_request.job_directory = request.job_directory;
    vm_request.parent_disk_path = resources.parent_disk_path;
    vm_request.firmware = resources.firmware;
    vm_request.cpu_count = options.cpu_count;
    vm_request.memory_mib = options.memory_mib;
    vm_request.overlay_limit_bytes = options.overlay_limit_bytes;
    auto session = resources.provider->create(vm_request, cancellation);
    if (!session) {
        stage.fail(session.error(), "IBootCheckProvider::create");
        const auto* message = session.error().message == kProviderUnavailable ? kProviderUnavailable
                                                                              : kVmCreateFailed;
        return base::Result<void>::failure(stage_error(session.error().code, message));
    }
    resources.session = std::move(session).value();
    stage.note("vm_name", resources.session->info().vm_name);
    stage.note("provider_version", resources.session->info().provider_version);
    return base::Result<void>::success();
}

/// Boot success criteria, in order of authority:
///  1. The hypervisor's own guest heartbeat (Hyper-V Integration Services)
///     reporting the guest OS is up — no agent inside the image, and it cannot
///     be fooled by the early-boot spinner or a later crash.
///  2. Fallback for providers/images without that channel: sustained growth of
///     the differencing overlay past a threshold, after a minimum elapsed time,
///     with the overlay still growing recently — so firmware/boot-loader writes
///     and a hung or blue-screened guest (which stop writing) do not pass.
[[nodiscard]] base::Result<void> wait_boot_confirmation(TaskResources& resources,
                                                        const BootCheckHostOptions& options,
                                                        const base::CancellationToken& cancellation,
                                                        ScopedStage& stage) {
    // Overlay fallback tuning: only consult it once the guest is well past
    // early boot and the overlay is both large and still actively growing.
    constexpr auto kOverlayFallbackAfter = std::chrono::seconds(90);
    constexpr auto kOverlayGrowthWindow = std::chrono::seconds(20);
    constexpr std::uint64_t kOverlayGrowthDeltaBytes = 8ULL * 1024 * 1024;

    const auto overlay_path = path_from_utf8(resources.session->info().child_medium_path);
    const auto started = std::chrono::steady_clock::now();
    const auto deadline = started + std::chrono::milliseconds(options.boot_timeout_ms);
    auto next_state_poll = started;
    std::uint64_t overlay_bytes = 0;
    std::uint64_t window_baseline_bytes = 0;
    auto window_baseline_at = started;
    const auto fail = [&stage, &overlay_bytes](const base::ErrorCode code,
                                               const char* const message_code) {
        stage.note_bytes("overlay_bytes", overlay_bytes);
        const base::Error error = stage_error(code, message_code);
        stage.fail(error, "wait_boot_confirmation");
        return base::Result<void>::failure(error);
    };
    const auto confirmed = [&stage, &overlay_bytes](const char* const how) {
        stage.note_bytes("overlay_bytes", overlay_bytes);
        stage.note("boot_confirmed", how);
        return base::Result<void>::success();
    };
    std::mutex mutex;
    std::condition_variable_any changed;
    for (;;) {
        if (cancellation.stop_requested()) {
            return fail(base::ErrorCode::kCancelled, kCancelled);
        }
        std::error_code file_error;
        const auto size = std::filesystem::file_size(overlay_path, file_error);
        if (!file_error) {
            overlay_bytes = size;
        }
        if (overlay_bytes > options.overlay_limit_bytes) {
            return fail(base::ErrorCode::kInsufficientSpace, kOverlayFull);
        }
        // The guest-channel heartbeat and the power state are both heavyweight
        // hypervisor queries, so they share the slower poll interval.
        const auto now = std::chrono::steady_clock::now();
        if (now >= next_state_poll) {
            next_state_poll = now + kVmStatePollInterval;
            auto heartbeat = resources.session->guest_heartbeat_ok(cancellation);
            if (!heartbeat) {
                return fail(base::ErrorCode::kCancelled, kCancelled);
            }
            if (heartbeat.value()) {
                return confirmed("guest_heartbeat");
            }
            auto state = resources.session->state(cancellation);
            const bool powered_off =
                state && (state.value() == virtualization::BootCheckVmState::kPoweredOff ||
                          state.value() == virtualization::BootCheckVmState::kAborted);
            if (powered_off) {
                return fail(base::ErrorCode::kIoFailure, kGuestPoweredOff);
            }
        }
        // Overlay fallback: once per growth window, measure how much the overlay
        // grew. Confirm only past the early-boot window, above the threshold, and
        // while still actively growing — a hung or blue-screened guest plateaus
        // and never clears the per-window delta.
        if (now - window_baseline_at >= kOverlayGrowthWindow) {
            const bool actively_growing =
                overlay_bytes >= window_baseline_bytes + kOverlayGrowthDeltaBytes;
            window_baseline_bytes = overlay_bytes;
            window_baseline_at = now;
            if (actively_growing && now - started >= kOverlayFallbackAfter &&
                overlay_bytes > options.boot_confirmed_overlay_bytes) {
                return confirmed("overlay_growth");
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return fail(base::ErrorCode::kIoFailure, kBootNotConfirmed);
        }
        std::unique_lock lock(mutex);
        changed.wait_for(lock, cancellation, kOverlayPollInterval, [] { return false; });
    }
}

[[nodiscard]] bool run_cleanup(TaskResources& resources, WorkerTaskLog* const log) {
    ScopedStage stage(log, "cleanup");
    CleanupDeadline deadline;
    bool clean = true;
    if (resources.session) {
        if (!resources.session->cleanup(deadline.token())) {
            clean = false;
        }
        resources.session.reset();
    }
    resources.provider.reset();
    if (resources.presentation) {
        resources.presentation->close();
        resources.presentation.reset();
    }
    if (resources.vhdx_presentation) {
        resources.vhdx_presentation->close();
        resources.vhdx_presentation.reset();
    }
    resources.disk.reset();
    resources.chain.reset();
    if (resources.owns_job_directory) {
        std::error_code error;
        std::filesystem::remove_all(resources.job_directory, error);
        if (error) {
            clean = false;
        }
    }
    if (!clean) {
        stage.fail(stage_error(base::ErrorCode::kIoFailure, kCleanupIncomplete), "cleanup");
    }
    return clean;
}

[[nodiscard]] base::Result<void>
run_presentation_stages(const contracts::BootCheckJobRequest& request,
                        const BootCheckHostOptions& options, const BootCheckHostContext& context,
                        const base::CancellationToken& cancellation, TaskResources& resources,
                        WorkerTaskLog* const log) {
    {
        ScopedStage stage(log, "validate_prerequisites");
        auto prepared = prepare_job_directory(resources, request.job_directory);
        if (!prepared) {
            stage.fail(prepared.error(), "prepare_job_directory");
            return prepared;
        }
    }
    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    {
        ScopedStage stage(log, "resolve_credentials");
        auto resolved = resolve_secrets(request, context.credentials, cancellation);
        if (!resolved) {
            stage.fail(resolved.error(), "resolve_secret");
            return base::Result<void>::failure(resolved.error());
        }
        secrets = std::move(resolved).value();
        stage.note_u64("layers", secrets.size());
    }
    {
        ScopedStage stage(log, "open_archive_chain");
        if (auto opened = open_archive_chain(request, options, secrets, resources, stage);
            !opened) {
            return opened;
        }
        secrets.clear();
    }
    {
        ScopedStage stage(log, "validate_boot_profile");
        if (auto valid = validate_boot_profile(resources, options, stage); !valid) {
            return valid;
        }
    }
    {
        ScopedStage stage(log, "select_provider");
        if (auto selected =
                select_provider(request.hypervisor, options, context, resources, cancellation, stage);
            !selected) {
            return selected;
        }
    }
    {
        ScopedStage stage(log, "present_disk");
        if (auto presented = present_disk(resources, context, cancellation, stage); !presented) {
            return presented;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> run_stages(const contracts::BootCheckJobRequest& request,
                                            const BootCheckHostOptions& options,
                                            const BootCheckHostContext& context,
                                            const base::CancellationToken& cancellation,
                                            TaskResources& resources, WorkerTaskLog* const log) {
    if (auto presented =
            run_presentation_stages(request, options, context, cancellation, resources, log);
        !presented) {
        return presented;
    }
    {
        ScopedStage stage(log, "create_vm");
        if (auto created = create_vm(request, options, resources, cancellation, stage); !created) {
            return created;
        }
    }
    {
        ScopedStage stage(log, "start_vm");
        if (auto started = resources.session->start(cancellation); !started) {
            stage.fail(started.error(), "startvm");
            return base::Result<void>::failure(stage_error(started.error().code, kVmStartFailed));
        }
    }
    {
        ScopedStage stage(log, "wait_boot_confirmation");
        return wait_boot_confirmation(resources, options, cancellation, stage);
    }
}

void log_request(WorkerTaskLog* const log, const contracts::BootCheckJobRequest& request,
                 const BootCheckHostOptions& options) {
    if (log == nullptr) {
        return;
    }
    log->section("Job");
    log->field("job_id", request.job_id);
    log->field("trace_id", request.trace_id);
    log->field("operation", "bootcheck");
    log->section("Request");
    log->field_u64("layers", request.source_refs.size());
    log->field("job_directory", request.job_directory);
    std::size_t empty_password_layers = 0;
    for (const auto& credential : request.credential_refs) {
        if (credential.value.empty()) {
            ++empty_password_layers;
        }
    }
    log->field_u64("password_layers", request.credential_refs.size() - empty_password_layers);
    log->field_u64("empty_password_layers", empty_password_layers);
    log->field_u64("cpu_count", options.cpu_count);
    log->field_u64("memory_mib", options.memory_mib);
    log->field_bytes("overlay_limit", options.overlay_limit_bytes);
    log->field_bytes("boot_confirmed_overlay", options.boot_confirmed_overlay_bytes);
    log->field_u64("boot_timeout_ms", options.boot_timeout_ms);
}

void log_result(WorkerTaskLog* const log, const contracts::TaskResult& result,
                const std::chrono::steady_clock::time_point started) {
    if (log == nullptr) {
        return;
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    log->section("Result");
    const char* outcome = "failed";
    if (result.outcome == contracts::TaskOutcome::kSucceeded ||
        result.outcome == contracts::TaskOutcome::kSucceededWithWarning) {
        outcome = result.warning_codes.empty() ? "succeeded" : "succeeded_with_warning";
    } else if (result.outcome == contracts::TaskOutcome::kCancelled) {
        outcome = "cancelled";
    }
    log->field("outcome", outcome);
    log->field("message_code", result.message_code);
    for (const auto& warning : result.warning_codes) {
        log->field("warning", warning);
    }
    if (result.error_code != base::ErrorCode::kNone) {
        log->field("error_code", base::error_code_name(result.error_code));
    }
    log->field("elapsed", apps::worker::format_duration_ms(elapsed));
}

[[nodiscard]] contracts::TaskResult make_task_result(const contracts::BootCheckJobRequest& request,
                                                     const base::Result<void>& outcome,
                                                     const std::uint64_t disk_bytes,
                                                     const bool clean) {
    contracts::TaskResult result;
    result.job_id = request.job_id;
    result.trace_id = request.trace_id;
    result.logical_bytes = disk_bytes;
    if (outcome) {
        result.outcome = clean ? contracts::TaskOutcome::kSucceeded
                               : contracts::TaskOutcome::kSucceededWithWarning;
        result.error_code = base::ErrorCode::kNone;
        result.message_code = kCompleted;
        if (!clean) {
            result.warning_codes.emplace_back(kCleanupIncomplete);
        }
        return result;
    }
    result.outcome = outcome.error().code == base::ErrorCode::kCancelled
                         ? contracts::TaskOutcome::kCancelled
                         : contracts::TaskOutcome::kFailed;
    result.error_code = outcome.error().code;
    // Stage errors carry their stable message code in Error::message.
    result.message_code = outcome.error().message.starts_with("bootcheck.")
                              ? outcome.error().message
                              : kBootNotConfirmed;
    if (!clean) {
        result.warning_codes.emplace_back(kCleanupIncomplete);
    }
    return result;
}

} // namespace

base::Result<contracts::TaskResult>
run_boot_check_task(const contracts::BootCheckJobRequest& request,
                    const BootCheckHostOptions& options, const BootCheckHostContext& context,
                    const base::CancellationToken& cancellation) {
    auto task_log = WorkerTaskLog::open("bootcheck", request.job_id);
    WorkerTaskLogScope log_scope(task_log.get());
    const auto started = std::chrono::steady_clock::now();
    log_request(task_log.get(), request, options);

    base::Result<void> outcome = base::Result<void>::success();
    TaskResources resources;
    if (cancellation.stop_requested()) {
        outcome = base::Result<void>::failure(stage_error(base::ErrorCode::kCancelled, kCancelled));
    } else {
        outcome = run_stages(request, options, context, cancellation, resources, task_log.get());
    }
    const std::uint64_t disk_bytes = resources.disk ? resources.disk->size_bytes() : 0;
    const bool clean = run_cleanup(resources, task_log.get());

    auto result = make_task_result(request, outcome, disk_bytes, clean);
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    log_result(task_log.get(), result, started);
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

base::Result<contracts::TaskResult> run_boot_check_present_hold(
    const contracts::BootCheckJobRequest& request, const BootCheckHostOptions& options,
    const BootCheckHostContext& context, const std::chrono::minutes hold_duration,
    const base::CancellationToken& cancellation) {
    auto task_log = WorkerTaskLog::open("bootcheck", request.job_id);
    WorkerTaskLogScope log_scope(task_log.get());
    const auto started = std::chrono::steady_clock::now();
    log_request(task_log.get(), request, options);

    base::Result<void> outcome = base::Result<void>::success();
    TaskResources resources;
    if (cancellation.stop_requested()) {
        outcome = base::Result<void>::failure(stage_error(base::ErrorCode::kCancelled, kCancelled));
    } else {
        outcome = run_presentation_stages(request, options, context, cancellation, resources,
                                          task_log.get());
    }
    if (outcome) {
        // Diagnostic hold: keep the read-only VMDK mounted so an external
        // hypervisor or tool can open it; cancellation ends the hold early.
        ScopedStage stage(task_log.get(), "present_hold");
        stage.note("parent_disk", resources.parent_disk_path);
        stage.note_u64("hold_minutes", static_cast<std::uint64_t>(hold_duration.count()));
        std::mutex mutex;
        std::condition_variable_any changed;
        std::unique_lock lock(mutex);
        changed.wait_for(lock, cancellation, hold_duration, [] { return false; });
    }
    const std::uint64_t disk_bytes = resources.disk ? resources.disk->size_bytes() : 0;
    const bool clean = run_cleanup(resources, task_log.get());

    auto result = make_task_result(request, outcome, disk_bytes, clean);
    if (outcome) {
        result.message_code = kPresentHoldCompleted;
    }
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    log_result(task_log.get(), result, started);
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

} // namespace aegra::apps::boot_check::detail
