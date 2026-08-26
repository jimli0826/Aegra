#include "pe_restore_job_service.h"

#include "worker_job_service_detail.h"
#include "worker_job_service_restore_shared.h"

#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_pe/archive_location.h"
#include "aegra/contracts/pe_restore.h"

#include <Windows.h>

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using worker_job_detail::acknowledgement;
using worker_job_detail::DiskRestoreChain;
using worker_job_detail::find_inventory_item;
using worker_job_detail::is_disk_target_id;
using worker_job_detail::load_restore_chain_or_fail;
using worker_job_detail::make_disk_restore_fingerprint;
using worker_job_detail::parse_disk_restore_fingerprint;
using worker_job_detail::persist_restore_preflight;
using worker_job_detail::PreparedRestoreChain;
using worker_job_detail::random_id;
using worker_job_detail::resolve_archive_absolute_path;
using worker_job_detail::source_disk_size_from_archive;

/// Closed payload injected beside the executor. Every entry is required: a
/// missing file fails Arm before DISM work. No debug CRT and no zlib.
inline constexpr const char* kPayloadCandidates[] = {
    "aegra_pe_restore.exe",
    "aegra_personal_worker.exe",
    "libsodium.dll",
    "zstd.dll",
    "vcruntime140.dll",
    "vcruntime140_1.dll",
    "msvcp140.dll",
    "msvcp140_1.dll",
    "msvcp140_2.dll",
    "msvcp140_atomic_wait.dll",
    "msvcp140_codecvt_ids.dll",
    "concrt140.dll",
};
inline constexpr const char* kExecutorFileName = "aegra_pe_restore.exe";

[[nodiscard]] bool file_exists_utf8(const std::string& path_utf8) {
    const auto required =
        MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wide.data(), required) <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(required) - 1);
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

[[nodiscard]] base::Result<std::vector<ports::PeImagePayloadFile>>
collect_payload(const std::string& payload_directory) {
    std::vector<ports::PeImagePayloadFile> payload;
    for (const char* file_name : kPayloadCandidates) {
        std::string source = payload_directory;
        if (!source.empty() && source.back() != '\\') {
            source.push_back('\\');
        }
        source += file_name;
        if (!file_exists_utf8(source)) {
            std::string message = "pe image payload file is missing: ";
            message += file_name;
            return base::Result<std::vector<ports::PeImagePayloadFile>>::failure(
                {base::ErrorCode::kNotFound, std::move(message)});
        }
        payload.push_back({std::move(source), file_name});
    }
    return base::Result<std::vector<ports::PeImagePayloadFile>>::success(std::move(payload));
}

[[nodiscard]] std::string trimmed(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && value[start] == ' ') {
        ++start;
    }
    return value.substr(start);
}

struct InspectedTarget final {
    std::string serial;
    std::uint64_t disk_size_bytes{0};
    std::string partition_style; // "gpt" | "mbr"
    std::string friendly_name;
};

/// Serial + geometry + partition style of the target disk; refuses disks the PE
/// executor could not re-match (empty serial, RAW style).
[[nodiscard]] base::Result<InspectedTarget> inspect_target(const std::uint32_t disk_number) {
    auto layout = adapters::windows_disk::inspect_physical_disk_layout(disk_number);
    if (!layout) {
        return base::Result<InspectedTarget>::failure(layout.error());
    }
    InspectedTarget inspected;
    inspected.serial = trimmed(layout.value().serial);
    if (inspected.serial.empty()) {
        return base::Result<InspectedTarget>::failure(
            {base::ErrorCode::kConflict,
             "target disk has no serial number; pe restore cannot re-match it"});
    }
    inspected.disk_size_bytes = layout.value().disk_size_bytes;
    if (layout.value().partition_style == "GPT") {
        inspected.partition_style = "gpt";
    } else if (layout.value().partition_style == "MBR") {
        inspected.partition_style = "mbr";
    } else {
        return base::Result<InspectedTarget>::failure(
            {base::ErrorCode::kConflict, "target disk partition style is unsupported"});
    }
    inspected.friendly_name = trimmed(layout.value().model);
    return base::Result<InspectedTarget>::success(std::move(inspected));
}

/// Resolves one archive layer and refuses when it lives on the restore target
/// (design §7: the restore would destroy its own source).
[[nodiscard]] base::Result<contracts::PeChainLayer>
resolve_chain_layer(const std::string& locator, const std::string& archive_key,
                    const std::string& file_uuid, const std::uint32_t target_disk_number) {
    auto absolute = resolve_archive_absolute_path(locator, archive_key);
    if (!absolute) {
        return base::Result<contracts::PeChainLayer>::failure(absolute.error());
    }
    auto location = adapters::windows_pe::locate_pe_archive_layer(absolute.value());
    if (!location) {
        return base::Result<contracts::PeChainLayer>::failure(location.error());
    }
    if (std::ranges::find(location.value().disk_numbers, target_disk_number) !=
        location.value().disk_numbers.end()) {
        return base::Result<contracts::PeChainLayer>::failure(
            {base::ErrorCode::kConflict,
             "backup chain is stored on the restore target disk; move it elsewhere first"});
    }
    contracts::PeChainLayer layer;
    layer.file_uuid = file_uuid;
    layer.volume_guid = std::move(location.value().volume_guid_utf8);
    layer.relative_path = std::move(location.value().relative_path_utf8);
    layer.online_path = std::move(absolute).value();
    return base::Result<contracts::PeChainLayer>::success(std::move(layer));
}

[[nodiscard]] base::Result<contracts::SourceInventoryItem>
find_system_disk_target(application::ISourceInventoryQuery& inventory,
                        const std::string_view target_source_id,
                        const base::CancellationToken cancellation) {
    if (!is_disk_target_id(target_source_id)) {
        return base::Result<contracts::SourceInventoryItem>::failure(
            {base::ErrorCode::kInvalidArgument, "pe restore target must be disk.N"});
    }
    auto target = find_inventory_item(inventory, target_source_id, cancellation);
    if (!target) {
        return target;
    }
    if (!target.value().is_system) {
        return base::Result<contracts::SourceInventoryItem>::failure(
            {base::ErrorCode::kConflict,
             "pe restore targets the system disk; use the online restore path otherwise"});
    }
    if (target.value().availability != contracts::SourceAvailability::kAvailable) {
        return base::Result<contracts::SourceInventoryItem>::failure(
            {base::ErrorCode::kConflict, "restore target is unavailable"});
    }
    return target;
}

struct ArmContext final {
    ports::RestorePreflightRecord record;
    DiskRestoreChain chain;
    contracts::SourceInventoryItem target;
    std::string locator;
};

struct ArmDependencies final {
    application::PeRestorePrepareService* prepare_service{nullptr};
    ports::IRandomSource* random{nullptr};
    const PeRestoreEnvironment* environment{nullptr};
    IServiceLog* logger{nullptr};
};

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
arm_with_context(const ArmDependencies& dependencies,
                 const contracts::ArmPeRestoreCommand& command, const ArmContext& context,
                 const base::CancellationToken cancellation) {
    using Output = base::Result<contracts::CommandAcknowledgement>;
    auto inspected = inspect_target(context.target.disk_number);
    if (!inspected) {
        return Output::failure(inspected.error());
    }
    if (inspected.value().disk_size_bytes < context.chain.disk_size_bytes) {
        return Output::failure(
            {base::ErrorCode::kConflict, "restore target is smaller than the source disk"});
    }
    auto job_uuid = random_id("pejob-", *dependencies.random, cancellation);
    if (!job_uuid) {
        return Output::failure(job_uuid.error());
    }
    application::PeRestorePrepareRequest request;
    request.job_uuid = job_uuid.value();
    request.product_version = dependencies.environment->product_version;
    request.data_dir_utf8 = dependencies.environment->data_dir_utf8;
    request.boot_entry_name = dependencies.environment->boot_entry_name;
    request.executor_target_name = kExecutorFileName;
    request.debug_shell = dependencies.environment->debug_shell;
    auto payload = collect_payload(dependencies.environment->payload_directory_utf8);
    if (!payload) {
        return Output::failure(payload.error());
    }
    request.payload = std::move(payload).value();
    request.source.repository_uuid = context.record.repository_uuid;
    request.source.source_disk_number = context.chain.source_disk_number;
    request.source.disk_size_bytes = context.chain.disk_size_bytes;
    request.source.chain_fingerprint = context.record.chain_fingerprint;
    request.source.chain.reserve(context.chain.layers.size());
    for (const auto& layer : context.chain.layers) {
        auto resolved = resolve_chain_layer(context.locator, layer.archive_key, layer.file_uuid,
                                            context.target.disk_number);
        if (!resolved) {
            return Output::failure(resolved.error());
        }
        request.source.chain.push_back(std::move(resolved).value());
    }
    request.target.serial_number = inspected.value().serial;
    request.target.size_bytes = inspected.value().disk_size_bytes;
    request.target.friendly_name = inspected.value().friendly_name;
    request.target.partition_style = inspected.value().partition_style;
    request.options.preserve_disk_signature = command.preserve_disk_signature;
    request.options.auto_expand_last_partition = command.auto_expand_last_partition;
    request.ui.locale = command.locale.empty() ? "en-US" : command.locale;
    request.ui.auto_start_seconds = 10;
    request.archive_password = command.archive_password;
    request.prompt_for_password = command.prompt_for_password;
    auto armed = dependencies.prepare_service->prepare_and_arm(request, cancellation);
    if (!request.archive_password.empty()) {
        SecureZeroMemory(request.archive_password.data(), request.archive_password.size());
    }
    if (!armed) {
        return Output::failure(armed.error());
    }
    if (dependencies.logger != nullptr) {
        dependencies.logger->write(ServiceLogLevel::kInfo, "pe_restore.armed",
                                   "job=" + job_uuid.value() +
                                       "; target=" + context.record.target_source_id);
    }
    return Output::success(acknowledgement(
        job_uuid.value(), contracts::CommandDisposition::kAccepted, job_uuid.value()));
}

} // namespace

PeRestoreJobService::PeRestoreJobService(application::ISourceInventoryQuery& source_inventory,
                                         ports::IControlPlaneDatabase& control_plane,
                                         ports::IRepositoryStorageFactory& storage_factory,
                                         application::PeRestorePrepareService& prepare_service,
                                         ports::IPePendingJobStore& pending_store,
                                         ports::IClock& clock, ports::IRandomSource& random,
                                         PeRestoreEnvironment environment,
                                         IServiceLog* logger) noexcept
    : source_inventory_(source_inventory), control_plane_(control_plane),
      storage_factory_(storage_factory), prepare_service_(prepare_service),
      pending_store_(pending_store), clock_(clock), random_(random),
      environment_(std::move(environment)), logger_(logger) {}

base::Result<contracts::RestorePreflight>
PeRestoreJobService::prepare_pe_restore(const contracts::RestorePreflightRequest& request,
                                        const base::CancellationToken cancellation) {
    using Output = base::Result<contracts::RestorePreflight>;
    if (auto valid = contracts::validate_restore_preflight_request(request); !valid) {
        return Output::failure(valid.error());
    }
    if (request.volume_size_policy != contracts::VolumeSizePolicy::kRequireSourceSize) {
        return Output::failure({base::ErrorCode::kInvalidArgument,
                                "pe restore requires volume_size_policy require_source_size"});
    }
    auto target = find_system_disk_target(source_inventory_, request.target_source_id,
                                          cancellation);
    if (!target) {
        return Output::failure(target.error());
    }
    const auto target_capacity = target.value().disk_capacity_bytes > 0
                                     ? target.value().disk_capacity_bytes
                                     : target.value().capacity_bytes;
    auto inspected = inspect_target(target.value().disk_number);
    if (!inspected) {
        return Output::failure(inspected.error());
    }
    auto chain_entries = load_restore_chain_or_fail(control_plane_, storage_factory_,
                                                    request.repository_connection_id,
                                                    request.recovery_point_id, cancellation);
    if (!chain_entries) {
        return Output::failure(chain_entries.error());
    }
    auto repository = control_plane_.get_repository_connection(request.repository_connection_id,
                                                               cancellation);
    if (!repository || !repository.value()) {
        return Output::failure(!repository ? repository.error()
                                           : base::Error{base::ErrorCode::kNotFound,
                                                         "repository connection not found"});
    }
    const auto& locator = repository.value()->locator;
    auto tip_path = resolve_archive_absolute_path(locator,
                                                  chain_entries.value().back().archive_main_key);
    if (!tip_path) {
        return Output::failure(tip_path.error());
    }
    auto disk_size = source_disk_size_from_archive(tip_path.value(), request.source_disk_number,
                                                   request.archive_password);
    if (!disk_size) {
        return Output::failure(disk_size.error());
    }
    if (target_capacity < disk_size.value() ||
        inspected.value().disk_size_bytes < disk_size.value()) {
        return Output::failure(
            {base::ErrorCode::kConflict, "restore target is smaller than the source disk"});
    }
    DiskRestoreChain chain;
    chain.source_disk_number = request.source_disk_number;
    chain.disk_size_bytes = disk_size.value();
    chain.layers.reserve(chain_entries.value().size());
    for (const auto& entry : chain_entries.value()) {
        // Layer-by-layer: every archive must be readable and off the target disk.
        auto layer = resolve_chain_layer(locator, entry.archive_main_key, entry.file_uuid,
                                         target.value().disk_number);
        if (!layer) {
            return Output::failure(layer.error());
        }
        chain.layers.push_back({entry.archive_main_key, entry.file_uuid});
    }
    PreparedRestoreChain prepared;
    prepared.repository_uuid = chain_entries.value().back().repository_uuid;
    prepared.chain_depth = static_cast<std::uint32_t>(chain_entries.value().size());
    prepared.target_capacity_bytes = target_capacity;
    prepared.logical_size_bytes = disk_size.value();
    prepared.chain_fingerprint = make_disk_restore_fingerprint(chain);
    prepared.volume_size_policy = contracts::VolumeSizePolicy::kRequireSourceSize;
    prepared.feasibility = contracts::RestoreFeasibility::kEligible;
    prepared.minimum_target_bytes = disk_size.value();
    prepared.message_code = "pe_restore.preflight_ready";
    return persist_restore_preflight(control_plane_, clock_, random_, request,
                                     std::move(prepared), cancellation);
}

base::Result<contracts::CommandAcknowledgement>
PeRestoreJobService::arm_pe_restore(const contracts::ArmPeRestoreCommand& command,
                                    const base::CancellationToken cancellation) {
    using Output = base::Result<contracts::CommandAcknowledgement>;
    if (auto valid = contracts::validate_arm_pe_restore_command(command); !valid) {
        return Output::failure(valid.error());
    }
    auto preflight = control_plane_.get_restore_preflight(command.preflight_token, cancellation);
    if (!preflight) {
        return Output::failure(preflight.error());
    }
    if (!preflight.value()) {
        return Output::failure({base::ErrorCode::kNotFound, "restore preflight was not found"});
    }
    ArmContext context;
    context.record = *preflight.value();
    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    if (now >= context.record.expires_utc_ms) {
        return Output::failure({base::ErrorCode::kConflict, "restore preflight has expired"});
    }
    auto chain = parse_disk_restore_fingerprint(context.record.chain_fingerprint);
    if (!chain) {
        return Output::failure(chain.error());
    }
    context.chain = std::move(chain).value();
    if (context.chain.layers.empty() ||
        context.chain.layers.back().file_uuid != context.record.recovery_point_id) {
        return Output::failure(
            {base::ErrorCode::kConflict, "restore preflight fingerprint is corrupt"});
    }
    auto target = find_system_disk_target(source_inventory_, context.record.target_source_id,
                                          cancellation);
    if (!target) {
        return Output::failure(target.error());
    }
    context.target = std::move(target).value();
    auto repository = control_plane_.get_repository_connection(
        context.record.repository_connection_id, cancellation);
    if (!repository || !repository.value()) {
        return Output::failure(!repository ? repository.error()
                                           : base::Error{base::ErrorCode::kNotFound,
                                                         "repository connection not found"});
    }
    context.locator = repository.value()->locator;
    // Revalidate the source geometry and the password (prompt mode carries neither).
    if (!command.prompt_for_password) {
        auto tip_path = resolve_archive_absolute_path(context.locator,
                                                      context.chain.layers.back().archive_key);
        if (!tip_path) {
            return Output::failure(tip_path.error());
        }
        auto disk_size = source_disk_size_from_archive(
            tip_path.value(), context.chain.source_disk_number, command.archive_password);
        if (!disk_size) {
            return Output::failure(disk_size.error());
        }
        if (disk_size.value() != context.chain.disk_size_bytes) {
            return Output::failure(
                {base::ErrorCode::kConflict, "restore source disk size changed after preflight"});
        }
    }
    const ArmDependencies dependencies{&prepare_service_, &random_, &environment_, logger_};
    return arm_with_context(dependencies, command, context, cancellation);
}

base::Result<contracts::PeRestoreState>
PeRestoreJobService::query_state(const base::CancellationToken cancellation) {
    auto state = prepare_service_.query_state(cancellation);
    if (!state) {
        return base::Result<contracts::PeRestoreState>::failure(state.error());
    }
    contracts::PeRestoreState result;
    result.armed = state.value().armed;
    result.job_uuid = state.value().job_uuid;
    result.target_display = state.value().target_display;
    result.created_utc_ms = state.value().created_utc_ms;
    return base::Result<contracts::PeRestoreState>::success(std::move(result));
}

base::Result<contracts::CommandAcknowledgement>
PeRestoreJobService::cancel_pe_restore(const base::CancellationToken cancellation) {
    if (auto cancelled = prepare_service_.cancel(cancellation); !cancelled) {
        return base::Result<contracts::CommandAcknowledgement>::failure(cancelled.error());
    }
    if (logger_ != nullptr) {
        logger_->write(ServiceLogLevel::kInfo, "pe_restore.cancelled_pending", "");
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement("pe-restore", contracts::CommandDisposition::kAccepted, std::nullopt));
}

void PeRestoreJobService::publish_boot_result_events(
    const base::CancellationToken cancellation) noexcept {
    auto result = pending_store_.read_result(cancellation);
    if (!result || !result.value().has_value()) {
        return;
    }
    const auto& boot_result = *result.value();
    ports::AuditEventRecord record;
    auto event_id = random_id("evt-", random_, cancellation);
    if (!event_id) {
        return;
    }
    record.event_id = std::move(event_id).value();
    record.created_utc_ms = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    record.correlation_id = boot_result.job_uuid;
    switch (boot_result.status) {
    case contracts::PeRestoreStatus::kSuccess:
        record.severity = contracts::AuditSeverity::kInformation;
        record.message_code = "pe_restore.succeeded";
        break;
    case contracts::PeRestoreStatus::kCancelled:
        record.severity = contracts::AuditSeverity::kWarning;
        record.message_code = "pe_restore.cancelled";
        break;
    case contracts::PeRestoreStatus::kFailed:
        record.severity = contracts::AuditSeverity::kError;
        record.message_code = "pe_restore.failed";
        break;
    }
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return;
    }
    if (auto appended = unit.value()->audit_events().append(record, cancellation); !appended) {
        unit.value()->rollback();
        // Keep the result for the next startup so the event is not lost.
        return;
    }
    if (auto committed = unit.value()->commit(cancellation); !committed) {
        return;
    }
    if (logger_ != nullptr) {
        logger_->write(ServiceLogLevel::kInfo, record.message_code,
                       "job=" + boot_result.job_uuid + "; code=" + boot_result.error_code);
    }
    (void)pending_store_.clear_result();
    (void)prepare_service_.cancel(cancellation);
}

} // namespace aegra::apps::service
