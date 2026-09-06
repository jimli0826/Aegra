#include "cli_commands.h"

#include "cli_commands_internal.h"
#include "cli_exit.h"
#include "cli_format.h"
#include "cli_io.h"

#include <cctype>
#include <chrono>
#include <optional>
#include <thread>
#include <utility>
#include <variant>

namespace aegra::apps::cli {
namespace {

[[nodiscard]] bool is_terminal(const contracts::ServiceJobState state) noexcept {
    return state == contracts::ServiceJobState::kSucceeded ||
           state == contracts::ServiceJobState::kFailed ||
           state == contracts::ServiceJobState::kCancelled ||
           state == contracts::ServiceJobState::kInterrupted;
}

[[nodiscard]] int job_exit_code(const contracts::ServiceJobState state) noexcept {
    return state == contracts::ServiceJobState::kSucceeded ? kExitOk : kExitJob;
}

[[nodiscard]] std::optional<contracts::JobSummary> find_job(const contracts::JobPage& page,
                                                            const std::string& job_id) {
    for (const auto& item : page.items) {
        if (item.job_id == job_id) {
            return item;
        }
    }
    return std::nullopt;
}

[[nodiscard]] base::Result<std::optional<contracts::JobSummary>>
query_job(ServiceSession& session, const std::string& job_id, const contracts::JobListScope scope) {
    contracts::JobListRequest request;
    request.page.maximum_results = kCliPageSize;
    request.scope = scope;
    std::optional<std::string> token;
    std::size_t collected = 0;
    do {
        request.page.continuation_token = token;
        auto response = session.transact(contracts::ServiceRequestKind::kListJobs, request, false);
        if (!response) {
            return base::Result<std::optional<contracts::JobSummary>>::failure(response.error());
        }
        if (response.value().kind == contracts::ServiceResponseKind::kRequestFailed) {
            write_failed_response(response.value());
            return base::Result<std::optional<contracts::JobSummary>>::failure(
                {response.value().boundary_error_code, response.value().message_code});
        }
        const auto* page = std::get_if<contracts::JobPage>(&response.value().payload);
        if (page == nullptr) {
            return base::Result<std::optional<contracts::JobSummary>>::failure(
                {base::ErrorCode::kCorruptData, "job list payload is missing"});
        }
        if (auto matched = find_job(*page, job_id); matched) {
            return base::Result<std::optional<contracts::JobSummary>>::success(std::move(matched));
        }
        collected += page->items.size();
        auto next = advance_continuation(token, page->continuation_token, collected);
        if (!next) {
            return base::Result<std::optional<contracts::JobSummary>>::failure(next.error());
        }
        token = std::move(next).value();
    } while (token);
    return base::Result<std::optional<contracts::JobSummary>>::success(std::nullopt);
}

[[nodiscard]] int wait_for_job(ServiceSession& session, const Options& options,
                               const std::string& job_id) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(options.wait_timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        auto active = query_job(session, job_id, contracts::JobListScope::kActive);
        if (!active) {
            write_error_object(active.error());
            return kExitRequest;
        }
        if (active.value()) {
            if (!options.json) {
                print_job_line(*active.value());
            }
            if (is_terminal(active.value()->state)) {
                return job_exit_code(active.value()->state);
            }
        } else {
            auto terminal = query_job(session, job_id, contracts::JobListScope::kTerminal);
            if (!terminal) {
                write_error_object(terminal.error());
                return kExitRequest;
            }
            if (terminal.value()) {
                if (options.json) {
                    contracts::ServiceResponse response;
                    response.kind = contracts::ServiceResponseKind::kQueryResult;
                    response.request_kind = contracts::ServiceRequestKind::kListJobs;
                    response.boundary_error_code = base::ErrorCode::kNone;
                    response.message_code = "control_plane.ready";
                    response.request_id = job_id;
                    response.payload = contracts::JobPage{{*terminal.value()}, std::nullopt};
                    return emit_query(session, options, response) == kExitOk
                               ? job_exit_code(terminal.value()->state)
                               : kExitInternal;
                }
                print_job_line(*terminal.value());
                return job_exit_code(terminal.value()->state);
            }
        }
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    write_error("timed out waiting for job");
    return kExitJob;
}

[[nodiscard]] int emit_command(ServiceSession& session, const Options& options,
                               const contracts::ServiceResponse& response) {
    if (response.kind == contracts::ServiceResponseKind::kRequestFailed) {
        write_failed_response(response);
        return kExitRequest;
    }
    if (options.json) {
        return emit_query(session, options, response);
    }
    const auto* ack = std::get_if<contracts::CommandAcknowledgement>(&response.payload);
    if (ack == nullptr) {
        write_error("command acknowledgement is missing");
        return kExitRequest;
    }
    print_acknowledgement(*ack);
    return kExitOk;
}

} // namespace

int run_schedule_run(ServiceSession& session, const Options& options) {
    if (!options.id) {
        return fail_missing_id();
    }
    contracts::StartBackupCommand command;
    command.schedule_id = *options.id;
    command.backup_type = options.backup_type.value_or(contracts::BackupType::kIncremental);
    auto response =
        session.transact(contracts::ServiceRequestKind::kStartBackup, std::move(command), true);
    if (!response) {
        return fail_request(response.error());
    }
    const auto accepted = emit_command(session, options, response.value());
    if (accepted != kExitOk || !options.wait) {
        return accepted;
    }
    const auto* ack = std::get_if<contracts::CommandAcknowledgement>(&response.value().payload);
    if (ack == nullptr || !ack->resource_id) {
        write_error("start backup did not return a job id");
        return kExitRequest;
    }
    return wait_for_job(session, options, *ack->resource_id);
}

int run_schedule_delete(ServiceSession& session, const Options& options) {
    if (!options.id) {
        return fail_missing_id();
    }
    auto response = session.transact(contracts::ServiceRequestKind::kDeleteSchedule,
                                     contracts::ResourceRef{*options.id}, true);
    if (!response) {
        return fail_request(response.error());
    }
    return emit_command(session, options, response.value());
}

int run_job_cancel(ServiceSession& session, const Options& options) {
    if (!options.id) {
        return fail_missing_id();
    }
    auto response = session.transact(contracts::ServiceRequestKind::kCancelJob,
                                     contracts::ResourceRef{*options.id}, true);
    if (!response) {
        return fail_request(response.error());
    }
    return emit_command(session, options, response.value());
}

int run_job_wait(ServiceSession& session, const Options& options) {
    if (!options.id) {
        return fail_missing_id();
    }
    return wait_for_job(session, options, *options.id);
}

int run_mount_start(ServiceSession& session, const Options& options) {
    if (!options.connection_id || !options.recovery_point_id || !options.source_disk_number) {
        write_error("mount start requires --connection, --recovery-point, and "
                    "--source-disk-number");
        return kExitUsage;
    }
    auto drive_letter = options.preferred_drive_letter;
    if (drive_letter) {
        if (drive_letter->size() != 1) {
            write_error("--drive-letter must be one letter from D through Z");
            return kExitUsage;
        }
        (*drive_letter)[0] =
            static_cast<char>(std::toupper(static_cast<unsigned char>((*drive_letter)[0])));
        if ((*drive_letter)[0] < 'D' || (*drive_letter)[0] > 'Z') {
            write_error("--drive-letter must be one letter from D through Z");
            return kExitUsage;
        }
    }
    contracts::MountRecoveryPointCommand command;
    command.repository_connection_id = *options.connection_id;
    command.recovery_point_id = *options.recovery_point_id;
    command.source_disk_number = *options.source_disk_number;
    command.preferred_drive_letter = std::move(drive_letter);
    auto response = session.transact(contracts::ServiceRequestKind::kMountRecoveryPoint,
                                     std::move(command), true);
    if (!response) {
        return fail_request(response.error());
    }
    return emit_command(session, options, response.value());
}

int run_mount_unmount(ServiceSession& session, const Options& options) {
    if (!options.id) {
        return fail_missing_id();
    }
    auto response = session.transact(contracts::ServiceRequestKind::kUnmountSession,
                                     contracts::ResourceRef{*options.id}, true);
    if (!response) {
        return fail_request(response.error());
    }
    return emit_command(session, options, response.value());
}

int run_restore_run(ServiceSession& session, const Options& options) {
    if (!options.connection_id || !options.recovery_point_id || !options.target_source_id ||
        !options.source_disk_number || !options.confirmed_target_source_id) {
        write_error("restore run requires --connection, --recovery-point, --target, "
                    "--source-disk-number, and --confirm-target");
        return kExitUsage;
    }
    if (!options.target_source_id->starts_with("disk.") ||
        *options.confirmed_target_source_id != *options.target_source_id) {
        write_error("--confirm-target must exactly match the disk.N value passed to --target");
        return kExitUsage;
    }
    contracts::RestorePreflightRequest preflight_request;
    preflight_request.repository_connection_id = *options.connection_id;
    preflight_request.recovery_point_id = *options.recovery_point_id;
    preflight_request.target_source_id = *options.target_source_id;
    preflight_request.source_disk_number = *options.source_disk_number;
    auto prepared = session.transact(contracts::ServiceRequestKind::kPrepareRestore,
                                     std::move(preflight_request), false);
    if (!prepared) {
        return fail_request(prepared.error());
    }
    if (prepared.value().kind == contracts::ServiceResponseKind::kRequestFailed) {
        return emit_command(session, options, prepared.value());
    }
    const auto* preflight = std::get_if<contracts::RestorePreflight>(&prepared.value().payload);
    if (preflight == nullptr || !preflight->restore_eligible ||
        preflight->feasibility != contracts::RestoreFeasibility::kEligible) {
        write_error("restore preflight did not return an eligible target");
        return kExitRequest;
    }
    contracts::StartRestoreCommand command;
    command.preflight_token = preflight->preflight_token;
    command.confirmed = true;
    command.preserve_disk_signature = options.preserve_disk_signature;
    command.auto_expand_last_partition = options.auto_expand_last_partition;
    auto started =
        session.transact(contracts::ServiceRequestKind::kStartRestore, std::move(command), true);
    if (!started) {
        return fail_request(started.error());
    }
    const auto accepted = emit_command(session, options, started.value());
    if (accepted != kExitOk || !options.wait) {
        return accepted;
    }
    const auto* acknowledgement =
        std::get_if<contracts::CommandAcknowledgement>(&started.value().payload);
    if (acknowledgement == nullptr || !acknowledgement->resource_id) {
        write_error("start restore did not return a job id");
        return kExitRequest;
    }
    return wait_for_job(session, options, *acknowledgement->resource_id);
}

} // namespace aegra::apps::cli
