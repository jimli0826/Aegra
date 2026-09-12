#include "aegra/apps/service/recovery_point_check_recorder.h"

#include "aegra/apps/service/service_host.h"

#include <algorithm>
#include <chrono>
#include <string>
#include <thread>
#include <utility>

namespace aegra::apps::service {
namespace {

void write_log(IServiceLog* const logger, const ServiceLogLevel level, const std::string_view code,
               const std::string_view detail) noexcept {
    if (logger != nullptr) {
        logger->write(level, code, detail);
    }
}

// The single-connection lock may momentarily be held by a reader; retry briefly
// instead of dropping a durable status update.
[[nodiscard]] base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>
begin_unit_retry(ports::IControlPlaneDatabase& control_plane) {
    constexpr int kMaximumAttempts = 40;
    constexpr auto kRetryDelay = std::chrono::milliseconds(10);
    for (int attempt = 0;; ++attempt) {
        auto unit = control_plane.begin_unit_of_work({});
        if (unit || unit.error().code != base::ErrorCode::kConflict ||
            attempt >= kMaximumAttempts) {
            return unit;
        }
        std::this_thread::sleep_for(kRetryDelay);
    }
}

void store_records(ports::IControlPlaneDatabase& control_plane, IServiceLog* const logger,
                   const std::vector<ports::RecoveryPointCheckRecord>& records) {
    if (records.empty()) {
        return;
    }
    auto unit = begin_unit_retry(control_plane);
    if (!unit) {
        write_log(logger, ServiceLogLevel::kWarning, "recovery_point_check.record_failed",
                  "begin failed; error=" + std::string(base::error_code_name(unit.error().code)));
        return;
    }
    for (const auto& record : records) {
        if (auto stored = unit.value()->recovery_point_checks().upsert(record, {}); !stored) {
            unit.value()->rollback();
            write_log(logger, ServiceLogLevel::kWarning, "recovery_point_check.record_failed",
                      "upsert failed for " + record.recovery_point_id + "; error=" +
                          std::string(base::error_code_name(stored.error().code)));
            return;
        }
    }
    if (auto committed = unit.value()->commit({}); !committed) {
        write_log(logger, ServiceLogLevel::kWarning, "recovery_point_check.record_failed",
                  "commit failed; error=" +
                      std::string(base::error_code_name(committed.error().code)));
    }
}

[[nodiscard]] ports::RecoveryPointCheckRecord
make_record(const std::string_view connection_id, const std::string_view recovery_point_id,
            const contracts::JobOperation operation, const contracts::RecoveryPointCheckState state,
            const std::string_view message_code, const std::string_view job_id,
            const std::uint64_t completed_utc_ms) {
    ports::RecoveryPointCheckRecord record;
    record.repository_connection_id = std::string(connection_id);
    record.recovery_point_id = std::string(recovery_point_id);
    record.operation = operation;
    record.state = state;
    record.message_code = message_code.empty() ? std::string("job.failed") : std::string(message_code);
    record.job_id = std::string(job_id);
    record.completed_utc_ms = completed_utc_ms;
    return record;
}

} // namespace

contracts::RecoveryPointCheckState
check_state_for_job_state(const contracts::ServiceJobState state) noexcept {
    switch (state) {
    case contracts::ServiceJobState::kSucceeded:
        return contracts::RecoveryPointCheckState::kSucceeded;
    case contracts::ServiceJobState::kCancelled:
        return contracts::RecoveryPointCheckState::kCancelled;
    case contracts::ServiceJobState::kInterrupted:
        return contracts::RecoveryPointCheckState::kInterrupted;
    default:
        return contracts::RecoveryPointCheckState::kFailed;
    }
}

void record_verify_check_outcomes(ports::IControlPlaneDatabase& control_plane,
                                  IServiceLog* const logger,
                                  const std::string_view repository_connection_id,
                                  const std::vector<std::string>& recovery_point_ids,
                                  const std::string_view current_recovery_point_id,
                                  const std::string_view job_id,
                                  const contracts::ServiceJobState final_state,
                                  const std::string_view message_code,
                                  const std::uint64_t completed_utc_ms) {
    if (repository_connection_id.empty() || recovery_point_ids.empty() || job_id.empty()) {
        return;
    }
    const auto terminal = check_state_for_job_state(final_state);
    const auto current = std::ranges::find(recovery_point_ids, current_recovery_point_id);
    const bool position_known =
        terminal != contracts::RecoveryPointCheckState::kSucceeded && current != recovery_point_ids.end();
    std::vector<ports::RecoveryPointCheckRecord> records;
    records.reserve(recovery_point_ids.size());
    for (auto it = recovery_point_ids.begin(); it != recovery_point_ids.end(); ++it) {
        if (terminal == contracts::RecoveryPointCheckState::kSucceeded) {
            records.push_back(make_record(repository_connection_id, *it,
                                          contracts::JobOperation::kVerify, terminal,
                                          "verify.completed", job_id, completed_utc_ms));
            continue;
        }
        if (!position_known) {
            // Unknown stop position: the whole batch is reported with the terminal state.
            records.push_back(make_record(repository_connection_id, *it,
                                          contracts::JobOperation::kVerify, terminal, message_code,
                                          job_id, completed_utc_ms));
            continue;
        }
        if (it < current) {
            records.push_back(make_record(repository_connection_id, *it,
                                          contracts::JobOperation::kVerify,
                                          contracts::RecoveryPointCheckState::kSucceeded,
                                          "verify.completed", job_id, completed_utc_ms));
        } else if (it == current) {
            records.push_back(make_record(repository_connection_id, *it,
                                          contracts::JobOperation::kVerify, terminal, message_code,
                                          job_id, completed_utc_ms));
        }
        // Points after the stop position were never reached: keep their previous record.
    }
    store_records(control_plane, logger, records);
}

void record_boot_check_outcome(ports::IControlPlaneDatabase& control_plane,
                               IServiceLog* const logger,
                               const std::string_view repository_connection_id,
                               const std::string_view recovery_point_id,
                               const std::string_view job_id,
                               const contracts::RecoveryPointCheckState state,
                               const std::string_view message_code,
                               const std::uint64_t completed_utc_ms) {
    if (repository_connection_id.empty() || recovery_point_id.empty() || job_id.empty()) {
        return;
    }
    store_records(control_plane, logger,
                  {make_record(repository_connection_id, recovery_point_id,
                               contracts::JobOperation::kBootCheck, state, message_code, job_id,
                               completed_utc_ms)});
}

} // namespace aegra::apps::service
