#include "aegra/apps/service/schedule_engine.h"

#include "aegra/apps/service/schedule_execution_coordinator.h"
#include "aegra/apps/service/schedule_next_run.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/worker_job_service.h"
#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"

#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] std::uint64_t utc_ms(const ports::IClock& clock) {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0 : static_cast<std::uint64_t>(now);
}

void log_engine(IServiceLog* logger, const ServiceLogLevel level, const std::string_view code,
                const std::string_view detail) noexcept {
    if (logger == nullptr) {
        return;
    }
    logger->write(level, code, detail);
}

} // namespace

ScheduleEngine::ScheduleEngine(ports::IControlPlaneDatabase& control_plane,
                               IWorkerJobService& worker_jobs, ports::IClock& clock,
                               ScheduleExecutionCoordinator& coordinator,
                               IServiceLog* const logger) noexcept
    : control_plane_(control_plane), worker_jobs_(worker_jobs), clock_(clock),
      coordinator_(coordinator), logger_(logger) {}

ScheduleEngine::~ScheduleEngine() { stop(); }

void ScheduleEngine::start() {
    if (thread_.joinable()) {
        return;
    }
    thread_ = std::jthread([this](const std::stop_token stop) { run(stop); });
    log_engine(logger_, ServiceLogLevel::kInfo, "schedule.engine_started",
               "poll_interval_s=" + std::to_string(kPollInterval.count()));
}

void ScheduleEngine::stop() noexcept {
    if (!thread_.joinable()) {
        return;
    }
    thread_.request_stop();
    thread_.join();
    log_engine(logger_, ServiceLogLevel::kInfo, "schedule.engine_stopped", "status=stopped");
}

void ScheduleEngine::run(const std::stop_token stop) {
    while (!stop.stop_requested()) {
        scan_due(stop);
        for (int step = 0; step < static_cast<int>(kPollInterval.count()) && !stop.stop_requested();
             ++step) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
    }
}

void ScheduleEngine::scan_due(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return;
    }
    const auto now = utc_ms(clock_);
    contracts::ScheduleListRequest request;
    request.enabled = true;
    request.page.maximum_results = contracts::kMaximumServicePageResults;
    for (;;) {
        if (cancellation.stop_requested()) {
            return;
        }
        auto page = control_plane_.list_schedules(request, cancellation);
        if (!page) {
            log_engine(logger_, ServiceLogLevel::kWarning, "schedule.scan_failed",
                       page.error().message);
            return;
        }
        for (const auto& summary : page.value().items) {
            if (cancellation.stop_requested()) {
                return;
            }
            if (!summary.enabled || !summary.next_run_utc_ms.has_value()) {
                continue;
            }
            if (*summary.next_run_utc_ms > now) {
                continue;
            }
            fire_due(summary.schedule_id, *summary.next_run_utc_ms, cancellation);
        }
        if (!page.value().continuation_token.has_value() ||
            page.value().continuation_token->empty()) {
            return;
        }
        request.page.continuation_token = std::move(page.value().continuation_token);
    }
}

void ScheduleEngine::fire_due(const std::string_view schedule_id,
                              const std::uint64_t expected_due_utc_ms,
                              const base::CancellationToken cancellation) {
    // Serialize with ScheduleService upsert/delete so disabled or rescheduled records are not
    // started from a stale list snapshot.
    auto acquired = coordinator_.acquire(cancellation);
    if (!acquired) {
        if (acquired.error().code != base::ErrorCode::kCancelled) {
            log_engine(logger_, ServiceLogLevel::kWarning, "schedule.coordination_failed",
                       acquired.error().message);
        }
        return;
    }
    auto lock = std::move(acquired).value();
    const auto key = make_schedule_fire_idempotency_key(schedule_id, expected_due_utc_ms);
    auto started =
        worker_jobs_.start_scheduled_backup(schedule_id, expected_due_utc_ms, key, cancellation);
    if (!started) {
        const auto& error = started.error();
        if (error.code == base::ErrorCode::kCancelled) {
            return;
        }
        log_engine(logger_, ServiceLogLevel::kWarning, "schedule.fire_failed",
                   "schedule_id=" + std::string(schedule_id) + " detail=" + error.message);
        if (error.code == base::ErrorCode::kNotFound) {
            (void)advance_next_run(schedule_id, expected_due_utc_ms, cancellation);
        }
        return;
    }
    switch (started.value().outcome) {
    case ScheduledBackupOutcome::kSkipped:
        log_engine(logger_, ServiceLogLevel::kInfo, "schedule.fire_skipped",
                   "schedule_id=" + std::string(schedule_id) + " reason=" + started.value().detail);
        return;
    case ScheduledBackupOutcome::kDeferredCapacity:
        log_engine(logger_, ServiceLogLevel::kInfo, "schedule.fire_deferred",
                   "schedule_id=" + std::string(schedule_id) + " reason=worker_capacity");
        return;
    case ScheduledBackupOutcome::kAccepted:
    case ScheduledBackupOutcome::kReplayed:
        break;
    }
    std::string detail = "schedule_id=";
    detail += schedule_id;
    detail += " due_utc_ms=";
    detail += std::to_string(expected_due_utc_ms);
    if (started.value().job_id) {
        detail += " job_id=";
        detail += *started.value().job_id;
    }
    detail += started.value().outcome == ScheduledBackupOutcome::kReplayed
                  ? " disposition=replayed"
                  : " disposition=accepted";
    log_engine(logger_, ServiceLogLevel::kInfo, "schedule.fired", detail);
    if (!advance_next_run(schedule_id, expected_due_utc_ms, cancellation)) {
        log_engine(logger_, ServiceLogLevel::kWarning, "schedule.next_run_advance_failed",
                   "schedule_id=" + std::string(schedule_id));
    }
}

bool ScheduleEngine::advance_next_run(const std::string_view schedule_id,
                                      const std::uint64_t due_next_run,
                                      const base::CancellationToken cancellation) {
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return false;
    }
    auto existing = unit.value()->schedules().get(schedule_id, cancellation);
    if (!existing) {
        unit.value()->rollback();
        return false;
    }
    if (!existing.value()) {
        unit.value()->rollback();
        return true;
    }
    auto record = std::move(*existing.value());
    if (!record.enabled || !record.next_run_utc_ms.has_value() ||
        *record.next_run_utc_ms != due_next_run) {
        unit.value()->rollback();
        return true;
    }
    const auto advance_now_ms = utc_ms(clock_);
    record.next_run_utc_ms = compute_next_run_utc_ms(record.trigger, advance_now_ms);
    record.updated_utc_ms = advance_now_ms;
    auto upserted = unit.value()->schedules().upsert(record, cancellation);
    if (!upserted) {
        unit.value()->rollback();
        return false;
    }
    if (!unit.value()->commit(cancellation)) {
        return false;
    }
    log_engine(logger_, ServiceLogLevel::kInfo, "schedule.next_run_advanced",
               "schedule_id=" + std::string(schedule_id) +
                   " next_utc_ms=" + std::to_string(*record.next_run_utc_ms));
    return true;
}

} // namespace aegra::apps::service
