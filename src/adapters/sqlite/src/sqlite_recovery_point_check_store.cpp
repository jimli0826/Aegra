#include "sqlite_internal.h"

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr std::size_t kMaximumFieldBytes = 128;

[[nodiscard]] bool valid_field(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaximumFieldBytes;
}

[[nodiscard]] base::Result<void>
validate_recovery_point_check_record(const ports::RecoveryPointCheckRecord& record) {
    if (!valid_field(record.repository_connection_id) || !valid_field(record.recovery_point_id) ||
        !valid_field(record.message_code) || !valid_field(record.job_id) ||
        (record.operation != contracts::JobOperation::kVerify &&
         record.operation != contracts::JobOperation::kBootCheck) ||
        !contracts::is_known_recovery_point_check_state(record.state) ||
        record.completed_utc_ms >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "recovery point check record is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ports::RecoveryPointCheckRecord>
read_recovery_point_check(sqlite3_stmt* const stmt) {
    ports::RecoveryPointCheckRecord record;
    record.repository_connection_id = column_text_required(stmt, 0);
    record.recovery_point_id = column_text_required(stmt, 1);
    const auto operation = column_uint64(stmt, 2);
    const auto state = column_uint64(stmt, 3);
    if (operation > (std::numeric_limits<std::uint8_t>::max)() ||
        state > (std::numeric_limits<std::uint8_t>::max)()) {
        return base::Result<ports::RecoveryPointCheckRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "recovery point check record is corrupt"));
    }
    record.operation = static_cast<contracts::JobOperation>(operation);
    record.state = static_cast<contracts::RecoveryPointCheckState>(state);
    record.message_code = column_text_required(stmt, 4);
    record.job_id = column_text_required(stmt, 5);
    record.completed_utc_ms = column_uint64(stmt, 6);
    if (auto valid = validate_recovery_point_check_record(record); !valid) {
        return base::Result<ports::RecoveryPointCheckRecord>::failure(valid.error());
    }
    return base::Result<ports::RecoveryPointCheckRecord>::success(std::move(record));
}

} // namespace

RecoveryPointCheckStore::RecoveryPointCheckStore(SqliteControlPlaneState& state,
                                                 const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> RecoveryPointCheckStore::upsert(const ports::RecoveryPointCheckRecord& record,
                                                   const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return cancelled;
    }
    if (auto valid = validate_recovery_point_check_record(record); !valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT OR REPLACE INTO recovery_point_checks(repository_connection_id, "
        "recovery_point_id, operation, state, message_code, job_id, completed_utc_ms) "
        "VALUES(?,?,?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    int index = 1;
    if (auto bound = stmt.bind_text(index++, record.repository_connection_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.recovery_point_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(record.operation));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(record.state)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.message_code); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_text(index++, record.job_id); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(index++, static_cast<std::int64_t>(record.completed_utc_ms));
        !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

base::Result<std::vector<ports::RecoveryPointCheckRecord>>
RecoveryPointCheckStore::list(const std::string_view repository_connection_id,
                              const base::CancellationToken cancellation) {
    using Outcome = base::Result<std::vector<ports::RecoveryPointCheckRecord>>;
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return Outcome::failure(active.error());
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return Outcome::failure(cancelled.error());
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "SELECT repository_connection_id, recovery_point_id, operation, state, message_code, "
        "job_id, completed_utc_ms FROM recovery_point_checks "
        "WHERE repository_connection_id = ? ORDER BY recovery_point_id ASC, operation ASC");
    if (!statement) {
        return Outcome::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, repository_connection_id); !bound) {
        return Outcome::failure(bound.error());
    }
    std::vector<ports::RecoveryPointCheckRecord> records;
    for (;;) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return Outcome::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        auto record = read_recovery_point_check(statement.value().get());
        if (!record) {
            return Outcome::failure(record.error());
        }
        records.push_back(std::move(record).value());
    }
    return Outcome::success(std::move(records));
}

} // namespace aegra::adapters::sqlite::detail
