#include "sqlite_internal.h"

#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr int kMaximumClaimBatch = 64;

[[nodiscard]] base::Result<void> bind_action(SqliteStatement& stmt, int& index,
                                             const ports::PostBackupActionRecord& action) {
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(action.state)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(index++, action.child_job_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, action.message_code); !bound) {
        return bound;
    }
    return stmt.bind_int64(index++, static_cast<std::int64_t>(action.attempts));
}

} // namespace

PostBackupPlanStore::PostBackupPlanStore(SqliteControlPlaneState& state,
                                         const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> PostBackupPlanStore::upsert(const ports::PostBackupPlanRecord& record,
                                               const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return cancelled;
    }
    if (auto valid = validate_post_backup_plan_record(record); !valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT OR REPLACE INTO post_backup_plans(backup_job_id, schedule_id, recovery_point_id, "
        "repository_connection_id, verify_required, boot_check_required, boot_check_hypervisor, verify_state, "
        "verify_job_id, verify_message_code, verify_attempts, boot_check_state, boot_check_job_id, "
        "boot_check_message_code, boot_check_attempts, claim_owner, lease_expires_utc_ms, "
        "created_utc_ms, updated_utc_ms) VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    int index = 1;
    if (auto bound = stmt.bind_text(index++, record.backup_job_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.schedule_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.recovery_point_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.repository_connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, record.verify_required ? 1 : 0); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, record.boot_check_required ? 1 : 0); !bound) {
        return bound;
    }
    auto hypervisor_bound = record.boot_check_hypervisor
                                ? stmt.bind_int64(index++, static_cast<std::int64_t>(
                                                             *record.boot_check_hypervisor))
                                : stmt.bind_null(index++);
    if (!hypervisor_bound) {
        return hypervisor_bound;
    }
    if (auto bound = bind_action(stmt, index, record.verify); !bound) {
        return bound;
    }
    if (auto bound = bind_action(stmt, index, record.boot_check); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text_nullable(index++, record.claim_owner); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64_nullable(index++, record.lease_expires_utc_ms); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(record.created_utc_ms));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(record.updated_utc_ms));
        !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

base::Result<std::optional<ports::PostBackupPlanRecord>>
PostBackupPlanStore::get(const std::string_view backup_job_id,
                         const base::CancellationToken cancellation) {
    using Outcome = base::Result<std::optional<ports::PostBackupPlanRecord>>;
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return Outcome::failure(active.error());
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return Outcome::failure(cancelled.error());
    }
    const std::string sql = std::string(kSelectPostBackupPlanSql) + " WHERE backup_job_id = ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return Outcome::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, backup_job_id); !bound) {
        return Outcome::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return Outcome::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return Outcome::success(std::nullopt);
    }
    auto record = read_post_backup_plan(statement.value().get());
    if (!record) {
        return Outcome::failure(record.error());
    }
    return Outcome::success(std::move(record).value());
}

base::Result<std::vector<ports::PostBackupPlanRecord>>
PostBackupPlanStore::claim_incomplete(const ports::PostBackupPlanClaimRequest& request,
                                      const base::CancellationToken cancellation) {
    using Outcome = base::Result<std::vector<ports::PostBackupPlanRecord>>;
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return Outcome::failure(active.error());
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return Outcome::failure(cancelled.error());
    }
    if (request.claim_owner.empty() || request.maximum_plans == 0 ||
        request.lease_expires_utc_ms <= request.now_utc_ms) {
        return Outcome::failure(
            make_error(base::ErrorCode::kInvalidArgument, "post backup claim request is invalid"));
    }
    // Incomplete = a required action is still pending/running. The unit of work
    // serializes writers, so select-then-update per row is race-free.
    const std::string sql =
        std::string(kSelectPostBackupPlanSql) +
        " WHERE ((verify_required = 1 AND verify_state IN (1, 2))"
        " OR (boot_check_required = 1 AND boot_check_state IN (1, 2)))"
        " AND (claim_owner IS NULL OR claim_owner = ? OR lease_expires_utc_ms IS NULL"
        " OR lease_expires_utc_ms < ?)"
        " ORDER BY created_utc_ms ASC, backup_job_id ASC LIMIT ?";
    auto statement = SqliteStatement::prepare(state_.db, sql);
    if (!statement) {
        return Outcome::failure(statement.error());
    }
    auto& stmt = statement.value();
    const auto limit = static_cast<std::int64_t>(
        request.maximum_plans > kMaximumClaimBatch ? kMaximumClaimBatch : request.maximum_plans);
    if (auto bound = stmt.bind_text(1, request.claim_owner); !bound) {
        return Outcome::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64(2, static_cast<std::int64_t>(request.now_utc_ms)); !bound) {
        return Outcome::failure(bound.error());
    }
    if (auto bound = stmt.bind_int64(3, limit); !bound) {
        return Outcome::failure(bound.error());
    }
    std::vector<ports::PostBackupPlanRecord> claimed;
    for (;;) {
        auto stepped = stmt.step();
        if (!stepped) {
            return Outcome::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_post_backup_plan(stmt.get());
        if (!record) {
            return Outcome::failure(record.error());
        }
        claimed.push_back(std::move(record).value());
    }
    for (auto& record : claimed) {
        record.claim_owner = request.claim_owner;
        record.lease_expires_utc_ms = request.lease_expires_utc_ms;
        record.updated_utc_ms =
            request.now_utc_ms > record.updated_utc_ms ? request.now_utc_ms : record.updated_utc_ms;
        auto update = SqliteStatement::prepare(
            state_.db, "UPDATE post_backup_plans SET claim_owner = ?, lease_expires_utc_ms = ?, "
                       "updated_utc_ms = ? WHERE backup_job_id = ?");
        if (!update) {
            return Outcome::failure(update.error());
        }
        auto& update_stmt = update.value();
        if (auto bound = update_stmt.bind_text(1, request.claim_owner); !bound) {
            return Outcome::failure(bound.error());
        }
        if (auto bound =
                update_stmt.bind_int64(2, static_cast<std::int64_t>(request.lease_expires_utc_ms));
            !bound) {
            return Outcome::failure(bound.error());
        }
        if (auto bound =
                update_stmt.bind_int64(3, static_cast<std::int64_t>(record.updated_utc_ms));
            !bound) {
            return Outcome::failure(bound.error());
        }
        if (auto bound = update_stmt.bind_text(4, record.backup_job_id); !bound) {
            return Outcome::failure(bound.error());
        }
        auto stepped = update_stmt.step();
        if (!stepped) {
            return Outcome::failure(stepped.error());
        }
    }
    return Outcome::success(std::move(claimed));
}

} // namespace aegra::adapters::sqlite::detail
