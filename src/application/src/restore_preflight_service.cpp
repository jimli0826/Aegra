#include "aegra/application/restore_preflight_service.h"

#include "aegra/application/source_inventory_query.h"
#include "aegra/ports/clock.h"

#include "application_ids.h"

#include <limits>
#include <utility>

namespace aegra::application {
namespace {

constexpr std::uint64_t kRestorePreflightTtlMs = 5U * 60U * 1'000U;
constexpr std::size_t kMaximumStableValueBytes = 128;

[[nodiscard]] bool valid_snapshot(const RestoreChainSnapshot& snapshot) noexcept {
    return !snapshot.repository_uuid.empty() &&
           snapshot.repository_uuid.size() <= kMaximumStableValueBytes &&
           !snapshot.chain_fingerprint.empty() &&
           snapshot.chain_fingerprint.size() <= kMaximumStableValueBytes &&
           snapshot.logical_size_bytes > 0 &&
           snapshot.logical_size_bytes <=
               static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) &&
           snapshot.chain_depth > 0;
}

[[nodiscard]] base::Result<std::uint64_t> expiration_time(ports::IClock& clock) {
    const auto now = clock.now_utc_ms();
    if (now < 0 || static_cast<std::uint64_t>(now) >
                       static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
                           kRestorePreflightTtlMs) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInternal, "restore preflight clock is invalid"});
    }
    return base::Result<std::uint64_t>::success(static_cast<std::uint64_t>(now) +
                                                kRestorePreflightTtlMs);
}

[[nodiscard]] ports::RestorePreflightRecord
make_record(const contracts::RestorePreflightRequest& request, const RestoreChainSnapshot& chain,
            const ports::SourceInventoryRecord& target, std::string token,
            const std::uint64_t expires) {
    return {std::move(token),
            request.repository_connection_id,
            chain.repository_uuid,
            request.recovery_point_id,
            request.target_source_id,
            chain.chain_fingerprint,
            chain.logical_size_bytes,
            target.capacity_bytes,
            chain.chain_depth,
            expires - kRestorePreflightTtlMs,
            expires};
}

[[nodiscard]] base::Result<void> persist_record(ports::IControlPlaneDatabase& control_plane,
                                                const ports::RestorePreflightRecord& record,
                                                const base::CancellationToken cancellation) {
    auto unit = control_plane.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<void>::failure(unit.error());
    }
    if (auto inserted = unit.value()->restore_preflights().insert(record, cancellation);
        !inserted) {
        return inserted;
    }
    return unit.value()->commit(cancellation);
}

[[nodiscard]] contracts::RestorePreflight to_contract(const ports::RestorePreflightRecord& record) {
    return {record.preflight_token,   record.repository_connection_id, record.recovery_point_id,
            record.target_source_id,  record.logical_size_bytes,       record.target_capacity_bytes,
            record.chain_depth,       record.expires_utc_ms,           true,
            "restore.preflight_ready"};
}

} // namespace

RestorePreflightService::RestorePreflightService(ports::IControlPlaneDatabase& control_plane,
                                                 IRestoreChainInspector& chain_inspector,
                                                 ISourceInventoryQuery& source_inventory,
                                                 ports::IClock& clock,
                                                 ports::IRandomSource& random) noexcept
    : control_plane_(control_plane), chain_inspector_(chain_inspector),
      source_inventory_(source_inventory), clock_(clock), random_(random) {}

base::Result<contracts::RestorePreflight>
RestorePreflightService::prepare(const contracts::RestorePreflightRequest& request,
                                 const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_restore_preflight_request(request); !valid) {
        return base::Result<contracts::RestorePreflight>::failure(valid.error());
    }
    auto connection =
        control_plane_.get_repository_connection(request.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<contracts::RestorePreflight>::failure(connection.error());
    }
    if (!connection.value()) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kNotFound, "repository connection not found"});
    }
    auto chain =
        chain_inspector_.inspect(*connection.value(), request.recovery_point_id, cancellation);
    if (!chain) {
        return base::Result<contracts::RestorePreflight>::failure(chain.error());
    }
    if (!valid_snapshot(chain.value())) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "restore chain snapshot is invalid"});
    }
    auto target = source_inventory_.resolve_source(request.target_source_id, cancellation);
    if (!target) {
        return base::Result<contracts::RestorePreflight>::failure(target.error());
    }
    if (target.value().kind != contracts::SourceKind::kVolume ||
        target.value().capacity_bytes < chain.value().logical_size_bytes) {
        return base::Result<contracts::RestorePreflight>::failure(
            {base::ErrorCode::kConflict, "restore target is not eligible"});
    }
    auto expires = expiration_time(clock_);
    if (!expires) {
        return base::Result<contracts::RestorePreflight>::failure(expires.error());
    }
    auto token = detail::make_random_id("preflight-", random_, cancellation);
    if (!token) {
        return base::Result<contracts::RestorePreflight>::failure(token.error());
    }
    auto record = make_record(request, chain.value(), target.value(), std::move(token).value(),
                              expires.value());
    if (auto persisted = persist_record(control_plane_, record, cancellation); !persisted) {
        return base::Result<contracts::RestorePreflight>::failure(persisted.error());
    }
    return base::Result<contracts::RestorePreflight>::success(to_contract(record));
}

} // namespace aegra::application
