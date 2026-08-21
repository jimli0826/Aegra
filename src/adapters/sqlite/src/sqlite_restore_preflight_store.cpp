#include "sqlite_internal.h"

#include <utility>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr auto kSelectRestorePreflightSql =
    "SELECT preflight_token, repository_connection_id, repository_uuid, recovery_point_id, "
    "target_source_id, chain_fingerprint, logical_size_bytes, target_capacity_bytes, chain_depth, "
    "created_utc_ms, expires_utc_ms, volume_size_policy, feasibility, minimum_target_bytes, "
    "relocation_bytes, scratch_upper_bound_bytes, shrink_plan_digest, target_binding_digest "
    "FROM restore_preflights WHERE preflight_token = ?";

} // namespace

RestorePreflightStore::RestorePreflightStore(SqliteControlPlaneState& state,
                                             const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<void> RestorePreflightStore::insert(const ports::RestorePreflightRecord& record,
                                                 const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return cancelled;
    }
    if (auto valid = validate_restore_preflight_record(record); !valid) {
        return valid;
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO restore_preflights(preflight_token, repository_connection_id, "
        "repository_uuid, recovery_point_id, target_source_id, chain_fingerprint, "
        "logical_size_bytes, target_capacity_bytes, chain_depth, created_utc_ms, expires_utc_ms, "
        "volume_size_policy, feasibility, minimum_target_bytes, relocation_bytes, "
        "scratch_upper_bound_bytes, shrink_plan_digest, target_binding_digest) "
        "VALUES(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_text(1, record.preflight_token); !bound)
        return bound;
    if (auto bound = stmt.bind_text(2, record.repository_connection_id); !bound)
        return bound;
    if (auto bound = stmt.bind_text(3, record.repository_uuid); !bound)
        return bound;
    if (auto bound = stmt.bind_text(4, record.recovery_point_id); !bound)
        return bound;
    if (auto bound = stmt.bind_text(5, record.target_source_id); !bound)
        return bound;
    if (auto bound = stmt.bind_text(6, record.chain_fingerprint); !bound)
        return bound;
    if (auto bound = stmt.bind_int64(7, static_cast<std::int64_t>(record.logical_size_bytes));
        !bound)
        return bound;
    if (auto bound = stmt.bind_int64(8, static_cast<std::int64_t>(record.target_capacity_bytes));
        !bound)
        return bound;
    if (auto bound = stmt.bind_int64(9, static_cast<std::int64_t>(record.chain_depth)); !bound)
        return bound;
    if (auto bound = stmt.bind_int64(10, static_cast<std::int64_t>(record.created_utc_ms)); !bound)
        return bound;
    if (auto bound = stmt.bind_int64(11, static_cast<std::int64_t>(record.expires_utc_ms)); !bound)
        return bound;
    if (auto bound =
            stmt.bind_int64(12, static_cast<std::int64_t>(record.volume_size_policy));
        !bound)
        return bound;
    if (auto bound = stmt.bind_int64(13, static_cast<std::int64_t>(record.feasibility)); !bound)
        return bound;
    if (auto bound = stmt.bind_int64(14, static_cast<std::int64_t>(record.minimum_target_bytes));
        !bound)
        return bound;
    if (auto bound = stmt.bind_int64(15, static_cast<std::int64_t>(record.relocation_bytes));
        !bound)
        return bound;
    if (auto bound =
            stmt.bind_int64(16, static_cast<std::int64_t>(record.scratch_upper_bound_bytes));
        !bound)
        return bound;
    if (auto bound = stmt.bind_text(17, record.shrink_plan_digest); !bound)
        return bound;
    if (auto bound = stmt.bind_text(18, record.target_binding_digest); !bound)
        return bound;
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return replace_restore_preflight_entry_ids(state_.db, record.preflight_token, record.entry_ids);
}

base::Result<std::optional<ports::RestorePreflightRecord>>
RestorePreflightStore::get(const std::string_view preflight_token,
                           const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(active.error());
    }
    if (auto cancelled = check_cancelled(cancellation); !cancelled) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(
            cancelled.error());
    }
    auto statement = SqliteStatement::prepare(state_.db, kSelectRestorePreflightSql);
    if (!statement) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(
            statement.error());
    }
    if (auto bound = statement.value().bind_text(1, preflight_token); !bound) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(bound.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::success(std::nullopt);
    }
    auto record = read_restore_preflight(statement.value().get());
    if (!record) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(record.error());
    }
    auto entry_ids = load_restore_preflight_entry_ids(state_.db, preflight_token);
    if (!entry_ids) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(
            entry_ids.error());
    }
    record.value().entry_ids = std::move(entry_ids).value();
    // Re-validate after attaching entry_ids (file preflight requires them).
    if (auto valid = validate_restore_preflight_record(record.value()); !valid) {
        return base::Result<std::optional<ports::RestorePreflightRecord>>::failure(valid.error());
    }
    return base::Result<std::optional<ports::RestorePreflightRecord>>::success(
        std::move(record).value());
}

} // namespace aegra::adapters::sqlite::detail
