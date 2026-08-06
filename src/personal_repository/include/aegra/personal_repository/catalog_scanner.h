#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/object_storage.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace aegra::personal_repository {

struct CatalogScannerLimits final {
    CatalogCodecLimits codec;
    std::uint32_t maximum_catalog_objects{10'000};
    std::uint32_t maximum_tombstone_objects{1'000};
    std::uint64_t maximum_total_read_bytes{64ULL * 1024ULL * 1024ULL};
    std::uint32_t maximum_page_results{100};
};

struct CatalogScanRequest final {
    std::optional<std::string> continuation_token;
    std::uint32_t maximum_results{50};
};

struct CatalogRecoveryPoint final {
    CatalogEntry entry;
    ChainState chain_state{ChainState::kIncomplete};

    bool operator==(const CatalogRecoveryPoint&) const = default;
};

struct CatalogScanPage final {
    RepositoryDescriptor descriptor;
    std::vector<CatalogRecoveryPoint> recovery_points;
    std::optional<std::string> continuation_token;
};

/// Raw catalog entries after decode + identity checks (tombstones applied).
/// Does not require a globally valid recovery-point graph — used for incremental parent
/// selection where structural graph errors demote to Full instead of failing the scan.
struct CatalogEntriesLoad final {
    RepositoryDescriptor descriptor;
    std::vector<CatalogEntry> entries;
};

class RepositoryCatalogScanner final {
  public:
    RepositoryCatalogScanner(ports::IObjectReader& reader, ports::IPrefixEnumerator& enumerator,
                             CatalogScannerLimits limits = {});

    /// Decode catalog objects and apply deletion tombstones. Does not build RecoveryPointGraph.
    /// Hard failures: storage IO, descriptor/tombstone/entry JSON identity conflicts, size limits.
    [[nodiscard]] base::Result<CatalogEntriesLoad>
    load_entries(base::CancellationToken cancellation) const;

    /// Paged catalog view with per-entry chain_state. Requires a globally valid graph.
    [[nodiscard]] base::Result<CatalogScanPage> scan(const CatalogScanRequest& request,
                                                     base::CancellationToken cancellation) const;

  private:
    ports::IObjectReader& reader_;
    ports::IPrefixEnumerator& enumerator_;
    CatalogScannerLimits limits_;
};

} // namespace aegra::personal_repository
