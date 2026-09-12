#pragma once

#include "aegra/contracts/progress.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/control_plane.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::service {

class IServiceLog;

/// Persists the latest terminal Verify / BootCheck outcome per recovery point
/// (`recovery_point_checks`, ADR-0033) so the Repository page can show it after
/// a Desktop or Service restart and after job retention purged the job rows.
/// Recording never fails the owning job: errors are logged and dropped.

/// Verify batch: `recovery_point_ids` is the job's ordered source list and
/// `current_recovery_point_id` the item the Worker was on when it stopped
/// (from the last progress snapshot; empty when unknown). Points before the
/// current one succeeded, the current one takes the terminal state, points
/// after it were never reached and keep their previous record.
void record_verify_check_outcomes(ports::IControlPlaneDatabase& control_plane,
                                  IServiceLog* logger, std::string_view repository_connection_id,
                                  const std::vector<std::string>& recovery_point_ids,
                                  std::string_view current_recovery_point_id,
                                  std::string_view job_id, contracts::ServiceJobState final_state,
                                  std::string_view message_code, std::uint64_t completed_utc_ms);

void record_boot_check_outcome(ports::IControlPlaneDatabase& control_plane, IServiceLog* logger,
                               std::string_view repository_connection_id,
                               std::string_view recovery_point_id, std::string_view job_id,
                               contracts::RecoveryPointCheckState state,
                               std::string_view message_code, std::uint64_t completed_utc_ms);

/// Maps a terminal job state onto the recorded check state (Succeeded/Failed/
/// Cancelled/Interrupted). Non-terminal states map to Failed.
[[nodiscard]] contracts::RecoveryPointCheckState
check_state_for_job_state(contracts::ServiceJobState state) noexcept;

} // namespace aegra::apps::service
