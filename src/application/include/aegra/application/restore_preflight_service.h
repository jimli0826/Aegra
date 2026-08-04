#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/control_plane.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::ports {
class IClock;
class IRandomSource;
} // namespace aegra::ports

namespace aegra::application {

class ISourceInventoryQuery;

struct RestoreChainSnapshot final {
    std::string repository_uuid;
    std::string chain_fingerprint;
    std::uint64_t logical_size_bytes{0};
    std::uint32_t chain_depth{0};
};

// S5-backed boundary for resolving and authenticating a complete base-first chain. The
// fingerprint must cover every layer identity and storage generation used by Start revalidation.
class IRestoreChainInspector {
  public:
    IRestoreChainInspector() = default;
    virtual ~IRestoreChainInspector() = default;
    IRestoreChainInspector(const IRestoreChainInspector&) = delete;
    IRestoreChainInspector& operator=(const IRestoreChainInspector&) = delete;
    IRestoreChainInspector(IRestoreChainInspector&&) = delete;
    IRestoreChainInspector& operator=(IRestoreChainInspector&&) = delete;

    [[nodiscard]] virtual base::Result<RestoreChainSnapshot>
    inspect(const ports::RepositoryConnectionRecord& connection, std::string_view recovery_point_id,
            base::CancellationToken cancellation) = 0;
};

class IRestorePreflightService {
  public:
    IRestorePreflightService() = default;
    virtual ~IRestorePreflightService() = default;
    IRestorePreflightService(const IRestorePreflightService&) = delete;
    IRestorePreflightService& operator=(const IRestorePreflightService&) = delete;

    [[nodiscard]] virtual base::Result<contracts::RestorePreflight>
    prepare(const contracts::RestorePreflightRequest& request,
            base::CancellationToken cancellation) = 0;
};

class RestorePreflightService final : public IRestorePreflightService {
  public:
    RestorePreflightService(ports::IControlPlaneDatabase& control_plane,
                            IRestoreChainInspector& chain_inspector,
                            ISourceInventoryQuery& source_inventory, ports::IClock& clock,
                            ports::IRandomSource& random) noexcept;

    [[nodiscard]] base::Result<contracts::RestorePreflight>
    prepare(const contracts::RestorePreflightRequest& request,
            base::CancellationToken cancellation) override;

  private:
    ports::IControlPlaneDatabase& control_plane_;
    IRestoreChainInspector& chain_inspector_;
    ISourceInventoryQuery& source_inventory_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
};

} // namespace aegra::application
