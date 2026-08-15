#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"

#include <chrono>
#include <cstdint>
#include <string_view>
#include <thread>

namespace aegra::apps::service {

class IServiceLog;
class IWorkerJobService;
class ScheduleExecutionCoordinator;

/// Background due-schedule scanner for the personal Service host (S8).
/// Polls enabled schedules, starts backup for next_run <= now, then advances next_run.
/// Shares ScheduleExecutionCoordinator with ScheduleService to serialize mutate vs fire.
class ScheduleEngine final {
  public:
    static constexpr std::chrono::seconds kPollInterval{15};

    ScheduleEngine(ports::IControlPlaneDatabase& control_plane, IWorkerJobService& worker_jobs,
                   ports::IClock& clock, ScheduleExecutionCoordinator& coordinator,
                   IServiceLog* logger = nullptr) noexcept;
    ~ScheduleEngine();

    ScheduleEngine(const ScheduleEngine&) = delete;
    ScheduleEngine& operator=(const ScheduleEngine&) = delete;

    void start();
    void stop() noexcept;

  private:
    void run(std::stop_token stop);
    void scan_due(base::CancellationToken cancellation);
    void fire_due(std::string_view schedule_id, std::uint64_t expected_due_utc_ms,
                  base::CancellationToken cancellation);
    [[nodiscard]] bool advance_next_run(std::string_view schedule_id, std::uint64_t due_next_run,
                                        base::CancellationToken cancellation);

    ports::IControlPlaneDatabase& control_plane_;
    IWorkerJobService& worker_jobs_;
    ports::IClock& clock_;
    ScheduleExecutionCoordinator& coordinator_;
    IServiceLog* logger_;
    std::jthread thread_;
};

} // namespace aegra::apps::service
