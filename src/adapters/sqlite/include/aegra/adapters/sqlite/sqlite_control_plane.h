#pragma once

#include "aegra/ports/control_plane.h"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace aegra::adapters::sqlite {
namespace detail {
struct SqliteControlPlaneState;
}

enum class SqliteOpenMode : std::uint8_t {
    kOpenExisting = 1,
    kCreateIfMissing = 2,
};

struct SqliteControlPlaneOpenRequest final {
    std::filesystem::path database_path;
    SqliteOpenMode mode{SqliteOpenMode::kCreateIfMissing};
};

// Personal-edition SQLite control plane. Single-writer Service model.
// Stores repository connections (SecretRef only), jobs, schedules, and audit events.
// Never stores plaintext credentials, Chunk Index, Manifest, or Recovery Point authority.
class SqliteControlPlaneDatabase final : public ports::IControlPlaneDatabase {
  public:
    [[nodiscard]] static base::Result<std::unique_ptr<SqliteControlPlaneDatabase>>
    open(const SqliteControlPlaneOpenRequest& request);

    ~SqliteControlPlaneDatabase() override;
    SqliteControlPlaneDatabase(const SqliteControlPlaneDatabase&) = delete;
    SqliteControlPlaneDatabase& operator=(const SqliteControlPlaneDatabase&) = delete;
    SqliteControlPlaneDatabase(SqliteControlPlaneDatabase&&) = delete;
    SqliteControlPlaneDatabase& operator=(SqliteControlPlaneDatabase&&) = delete;

    [[nodiscard]] std::uint32_t schema_version() const noexcept override;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>>
    begin_unit_of_work(base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<std::optional<ports::RepositoryConnectionRecord>>
    get_repository_connection(std::string_view connection_id,
                              base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::RepositoryConnectionPage>
    list_repository_connections(const contracts::RepositoryConnectionListRequest& request,
                                base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::JobRecord>>
    get_job(std::string_view job_id, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::JobRecord>>
    get_job_by_idempotency_key(std::string_view idempotency_key,
                               base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::JobRecord>>
    get_job_by_preflight_token(std::string_view preflight_token,
                               base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::JobPage>
    list_jobs(const contracts::JobListRequest& request,
              base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::ScheduleRecord>>
    get_schedule(std::string_view schedule_id, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::SchedulePage>
    list_schedules(const contracts::ScheduleListRequest& request,
                   base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<contracts::AuditEventPage>
    list_audit_events(const contracts::AuditEventListRequest& request,
                      base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::CommandRecord>>
    get_command(std::string_view idempotency_key, base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::optional<ports::RestorePreflightRecord>>
    get_restore_preflight(std::string_view preflight_token,
                          base::CancellationToken cancellation) override;

  private:
    explicit SqliteControlPlaneDatabase(
        std::shared_ptr<detail::SqliteControlPlaneState> state) noexcept;

    std::shared_ptr<detail::SqliteControlPlaneState> state_;
};

} // namespace aegra::adapters::sqlite
