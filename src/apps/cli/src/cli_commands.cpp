#include "cli_commands.h"

#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/base/error.h"

#include "cli_commands_internal.h"
#include "cli_exit.h"
#include "cli_format.h"
#include "cli_io.h"

#include <variant>

namespace aegra::apps::cli {
namespace {

[[nodiscard]] int dispatch(ServiceSession& session, const Options& options) {
    switch (options.command) {
    case Command::kHelp:
        print_help();
        return kExitOk;
    case Command::kStatus:
        return run_status(session, options);
    case Command::kScheduleList:
        return run_schedule_list(session, options);
    case Command::kScheduleRun:
        return run_schedule_run(session, options);
    case Command::kScheduleDelete:
        return run_schedule_delete(session, options);
    case Command::kJobList:
        return run_job_list(session, options);
    case Command::kJobCancel:
        return run_job_cancel(session, options);
    case Command::kJobWait:
        return run_job_wait(session, options);
    case Command::kRepositoryList:
        return run_repository_list(session, options);
    case Command::kRecoveryPointList:
        return run_recovery_point_list(session, options);
    case Command::kInventoryList:
        return run_inventory_list(session, options);
    case Command::kEventList:
        return run_event_list(session, options);
    case Command::kMountList:
        return run_mount_list(session, options);
    case Command::kMountStart:
        return run_mount_start(session, options);
    case Command::kMountUnmount:
        return run_mount_unmount(session, options);
    case Command::kSettingsGet:
        return run_settings_get(session, options);
    case Command::kRestoreRun:
        return run_restore_run(session, options);
    }
    write_error("unknown command");
    return kExitUsage;
}

} // namespace

int emit_query(ServiceSession& session, const Options& options,
               const contracts::ServiceResponse& response) {
    if (response.kind == contracts::ServiceResponseKind::kRequestFailed) {
        write_failed_response(response);
        return kExitRequest;
    }
    if (!options.json) {
        return kExitOk;
    }
    auto printed = session.print_json(response);
    if (!printed) {
        write_error_object(printed.error());
        return kExitInternal;
    }
    return kExitOk;
}

int fail_missing_id() {
    write_error("missing --id");
    return kExitUsage;
}

int fail_request(const base::Error& error) {
    write_error_object(error);
    return kExitRequest;
}

base::Result<std::optional<std::string>>
advance_continuation(const std::optional<std::string>& previous,
                     const std::optional<std::string>& next, const std::size_t collected) {
    if (!next || collected >= kCliMaximumItems) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (previous && *previous == *next) {
        return base::Result<std::optional<std::string>>::failure(
            {base::ErrorCode::kCorruptData, "continuation token did not advance"});
    }
    return base::Result<std::optional<std::string>>::success(next);
}

int run_status(ServiceSession& session, const Options& options) {
    if (options.json) {
        contracts::ServiceResponse response;
        response.kind = contracts::ServiceResponseKind::kQueryResult;
        response.request_kind = contracts::ServiceRequestKind::kGetServiceInfo;
        response.boundary_error_code = base::ErrorCode::kNone;
        response.message_code = "service.ready";
        response.request_id = "cli-status";
        response.payload = session.info();
        return emit_query(session, options, response);
    }
    print_status(session.info());
    return kExitOk;
}

int run_settings_get(ServiceSession& session, const Options& options) {
    auto response = session.transact(contracts::ServiceRequestKind::kGetServiceSettings,
                                     contracts::ServiceSettingsQuery{}, false);
    if (!response) {
        return fail_request(response.error());
    }
    const auto json_exit = emit_query(session, options, response.value());
    if (json_exit != kExitOk || options.json) {
        return json_exit;
    }
    const auto* settings = std::get_if<contracts::ServiceSettings>(&response.value().payload);
    if (settings == nullptr) {
        write_error("settings payload is missing");
        return kExitRequest;
    }
    print_settings(*settings);
    return kExitOk;
}

int run_command(const Options& options) {
    if (options.command == Command::kHelp) {
        print_help();
        return kExitOk;
    }
    adapters::windows_system::WindowsCryptographicRandom random;
    auto session = ServiceSession::connect(options.timeout_ms, random);
    if (!session) {
        write_error_object(session.error());
        return kExitConnect;
    }
    return dispatch(*session.value(), options);
}

} // namespace aegra::apps::cli
