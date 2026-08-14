#include "network_aware_storage_factory.h"

#include "network_share_access.h"

namespace aegra::apps::service {
namespace {

[[nodiscard]] base::Result<std::optional<ports::RepositoryConnectionRecord>>
find_connection_for_locator(ports::IControlPlaneDatabase& control_plane,
                            const std::string_view locator,
                            const base::CancellationToken cancellation) {
    auto page = control_plane.list_repository_connections(
        {{contracts::kMaximumServicePageResults, std::nullopt}, std::nullopt}, cancellation);
    if (!page) {
        return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
            page.error());
    }
    std::optional<std::string> token = page.value().continuation_token;
    std::vector<std::string> ids;
    for (const auto& item : page.value().items) {
        ids.push_back(item.connection_id);
    }
    while (token) {
        auto next = control_plane.list_repository_connections(
            {{contracts::kMaximumServicePageResults, token}, std::nullopt}, cancellation);
        if (!next) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
                next.error());
        }
        for (const auto& item : next.value().items) {
            ids.push_back(item.connection_id);
        }
        token = next.value().continuation_token;
    }
    for (const auto& id : ids) {
        auto record = control_plane.get_repository_connection(id, cancellation);
        if (!record) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::failure(
                record.error());
        }
        if (record.value() && record.value()->locator == locator) {
            return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(
                std::move(record.value()));
        }
    }
    return base::Result<std::optional<ports::RepositoryConnectionRecord>>::success(std::nullopt);
}

} // namespace

base::Result<void>
NetworkAwareRepositoryStorageFactory::ensure_network_access(
    const std::string_view locator, const base::CancellationToken cancellation) {
    if (!is_unc_locator(locator)) {
        return base::Result<void>::success();
    }
    auto reachable = probe_network_share_server(locator, cancellation);
    if (!reachable) {
        return reachable;
    }
    auto found = find_connection_for_locator(control_plane_, locator, cancellation);
    if (!found) {
        return base::Result<void>::failure(found.error());
    }
    if (!found.value() || !found.value()->credential_ref) {
        // Guest/anonymous or first-time create (credentials applied by service_host).
        return base::Result<void>::success();
    }
    return connect_network_share_from_secret(locator, *found.value()->credential_ref);
}

base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
NetworkAwareRepositoryStorageFactory::open(const std::string_view locator,
                                           const base::CancellationToken cancellation) {
    auto ready = ensure_network_access(locator, cancellation);
    if (!ready) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            ready.error());
    }
    return inner_.open(locator, cancellation);
}

base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
NetworkAwareRepositoryStorageFactory::create_empty(const std::string_view locator,
                                                   const base::CancellationToken cancellation) {
    auto ready = ensure_network_access(locator, cancellation);
    if (!ready) {
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
            ready.error());
    }
    return inner_.create_empty(locator, cancellation);
}

} // namespace aegra::apps::service
