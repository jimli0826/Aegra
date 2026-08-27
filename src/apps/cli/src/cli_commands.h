#pragma once

#include "cli_args.h"
#include "cli_session.h"

namespace aegra::apps::cli {

[[nodiscard]] int run_command(const Options& options);
[[nodiscard]] int run_status(ServiceSession& session, const Options& options);
[[nodiscard]] int run_settings_get(ServiceSession& session, const Options& options);
[[nodiscard]] int run_schedule_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_repository_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_recovery_point_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_inventory_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_job_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_event_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_mount_list(ServiceSession& session, const Options& options);
[[nodiscard]] int run_schedule_run(ServiceSession& session, const Options& options);
[[nodiscard]] int run_schedule_delete(ServiceSession& session, const Options& options);
[[nodiscard]] int run_job_cancel(ServiceSession& session, const Options& options);
[[nodiscard]] int run_job_wait(ServiceSession& session, const Options& options);

} // namespace aegra::apps::cli
