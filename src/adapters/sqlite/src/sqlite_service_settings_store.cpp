#include "sqlite_internal.h"

#include <limits>
#include <utility>

namespace aegra::adapters::sqlite::detail {
ServiceSettingsStore::ServiceSettingsStore(SqliteControlPlaneState& state,
                                           const bool* const unit_of_work_active) noexcept
    : state_(state), unit_of_work_active_(unit_of_work_active) {}

base::Result<ports::ServiceSettingsRecord>
ServiceSettingsStore::get(const base::CancellationToken cancellation) {
    if (unit_of_work_active_ != nullptr) {
        if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
            return base::Result<ports::ServiceSettingsRecord>::failure(active.error());
        }
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return base::Result<ports::ServiceSettingsRecord>::failure(cancelled.error());
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "SELECT job_retention_months, default_boot_check_hypervisor, boot_check_cpu_count, "
        "boot_check_memory_mib, boot_check_concurrency, verify_scope, verify_concurrency, "
        "updated_utc_ms "
        "FROM service_settings WHERE id = 1");
    if (!statement) {
        return base::Result<ports::ServiceSettingsRecord>::failure(statement.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<ports::ServiceSettingsRecord>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        // Schema always seeds the row; missing row is an internal fault.
        return base::Result<ports::ServiceSettingsRecord>::failure(
            make_error(base::ErrorCode::kInternal, "service settings row is missing"));
    }
    ports::ServiceSettingsRecord record;
    record.job_retention_months =
        static_cast<std::uint8_t>(column_uint64(statement.value().get(), 0));
    record.default_boot_check_hypervisor =
        static_cast<contracts::BootCheckHypervisor>(column_uint64(statement.value().get(), 1));
    record.boot_check_cpu_count =
        static_cast<std::uint32_t>(column_uint64(statement.value().get(), 2));
    record.boot_check_memory_mib =
        static_cast<std::uint32_t>(column_uint64(statement.value().get(), 3));
    record.boot_check_concurrency =
        static_cast<std::uint32_t>(column_uint64(statement.value().get(), 4));
    record.verify_scope =
        static_cast<contracts::VerifyScope>(column_uint64(statement.value().get(), 5));
    record.verify_concurrency =
        static_cast<std::uint32_t>(column_uint64(statement.value().get(), 6));
    record.updated_utc_ms = column_uint64(statement.value().get(), 7);
    if (!contracts::is_valid_job_retention_months(record.job_retention_months) ||
        !contracts::is_known_boot_check_hypervisor(record.default_boot_check_hypervisor) ||
        record.boot_check_cpu_count < contracts::kMinimumBootCheckCpuCount ||
        record.boot_check_cpu_count > contracts::kMaximumBootCheckCpuCount ||
        record.boot_check_memory_mib < contracts::kMinimumBootCheckMemoryMib ||
        record.boot_check_memory_mib > contracts::kMaximumBootCheckMemoryMib ||
        record.boot_check_concurrency < contracts::kMinimumBootCheckConcurrency ||
        record.boot_check_concurrency > contracts::kMaximumBootCheckConcurrency ||
        !contracts::is_known_verify_scope(record.verify_scope) ||
        record.verify_concurrency < contracts::kMinimumVerifyConcurrency ||
        record.verify_concurrency > contracts::kMaximumVerifyConcurrency) {
        return base::Result<ports::ServiceSettingsRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "service settings record is invalid"));
    }
    return base::Result<ports::ServiceSettingsRecord>::success(std::move(record));
}

base::Result<void> ServiceSettingsStore::upsert(const ports::ServiceSettingsRecord& record,
                                                const base::CancellationToken cancellation) {
    if (auto active = check_unit_of_work_active(unit_of_work_active_); !active) {
        return active;
    }
    auto cancelled = check_cancelled(cancellation);
    if (!cancelled) {
        return cancelled;
    }
    if (!contracts::is_valid_job_retention_months(record.job_retention_months) ||
        !contracts::is_known_boot_check_hypervisor(record.default_boot_check_hypervisor) ||
        record.boot_check_cpu_count < contracts::kMinimumBootCheckCpuCount ||
        record.boot_check_cpu_count > contracts::kMaximumBootCheckCpuCount ||
        record.boot_check_memory_mib < contracts::kMinimumBootCheckMemoryMib ||
        record.boot_check_memory_mib > contracts::kMaximumBootCheckMemoryMib ||
        record.boot_check_concurrency < contracts::kMinimumBootCheckConcurrency ||
        record.boot_check_concurrency > contracts::kMaximumBootCheckConcurrency ||
        !contracts::is_known_verify_scope(record.verify_scope) ||
        record.verify_concurrency < contracts::kMinimumVerifyConcurrency ||
        record.verify_concurrency > contracts::kMaximumVerifyConcurrency ||
        record.updated_utc_ms >
            static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "service settings record is invalid"));
    }
    auto statement = SqliteStatement::prepare(
        state_.db,
        "INSERT INTO service_settings(id, job_retention_months, default_boot_check_hypervisor, "
        "boot_check_cpu_count, boot_check_memory_mib, boot_check_concurrency, verify_scope, "
        "verify_concurrency, updated_utc_ms) VALUES(1, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(id) DO UPDATE SET job_retention_months = excluded.job_retention_months, "
        "default_boot_check_hypervisor = excluded.default_boot_check_hypervisor, "
        "boot_check_cpu_count = excluded.boot_check_cpu_count, "
        "boot_check_memory_mib = excluded.boot_check_memory_mib, "
        "boot_check_concurrency = excluded.boot_check_concurrency, "
        "verify_scope = excluded.verify_scope, "
        "verify_concurrency = excluded.verify_concurrency, "
        "updated_utc_ms = excluded.updated_utc_ms");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto& stmt = statement.value();
    if (auto bound = stmt.bind_int64(1, static_cast<std::int64_t>(record.job_retention_months));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(
            2, static_cast<std::int64_t>(record.default_boot_check_hypervisor));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(3, static_cast<std::int64_t>(record.boot_check_cpu_count));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(4, static_cast<std::int64_t>(record.boot_check_memory_mib));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(5, static_cast<std::int64_t>(record.boot_check_concurrency));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(6, static_cast<std::int64_t>(record.verify_scope)); !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(7, static_cast<std::int64_t>(record.verify_concurrency));
        !bound) {
        return bound;
    }
    if (auto bound = stmt.bind_int64(8, static_cast<std::int64_t>(record.updated_utc_ms));
        !bound) {
        return bound;
    }
    auto stepped = stmt.step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::sqlite::detail
