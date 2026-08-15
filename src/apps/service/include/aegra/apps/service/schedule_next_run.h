#pragma once

#include "aegra/contracts/service_control.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::apps::service {

/// Next future fire time for a schedule trigger.
/// Minutes are applied on the UTC day grid (same rule as UpsertSchedule). Empty minute list
/// returns now_ms so callers can treat the schedule as immediately due for recompute.
[[nodiscard]] std::uint64_t compute_next_run_utc_ms(const contracts::ScheduleTrigger& trigger,
                                                    std::uint64_t now_ms);

/// Idempotency key for one scheduled fire slot: schedule_id + the due next_run value.
[[nodiscard]] std::string make_schedule_fire_idempotency_key(std::string_view schedule_id,
                                                            std::uint64_t due_next_run_utc_ms);

} // namespace aegra::apps::service
