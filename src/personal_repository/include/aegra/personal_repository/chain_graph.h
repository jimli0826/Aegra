#pragma once

#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::personal_repository {

enum class ChainState : std::uint8_t {
    kComplete = 1,
    kIncomplete = 2,
};

class RecoveryPointGraph final {
  public:
    [[nodiscard]] static base::Result<RecoveryPointGraph>
    build(std::vector<CatalogEntry> entries, std::uint32_t maximum_chain_depth = 128);

    [[nodiscard]] const CatalogEntry* find(std::string_view file_uuid) const noexcept;
    [[nodiscard]] base::Result<ChainState> chain_state(std::string_view file_uuid) const;
    [[nodiscard]] base::Result<std::vector<CatalogEntry>>
    resolve_chain(std::string_view file_uuid) const;

  private:
    explicit RecoveryPointGraph(std::map<std::string, CatalogEntry, std::less<>> entries,
                                std::uint32_t maximum_chain_depth) noexcept;

    std::map<std::string, CatalogEntry, std::less<>> entries_;
    std::uint32_t maximum_chain_depth_{0};
};

} // namespace aegra::personal_repository
