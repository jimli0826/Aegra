#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/source_inventory.h"

namespace aegra::application {

class ISourceInventoryQuery {
  public:
    ISourceInventoryQuery() = default;
    virtual ~ISourceInventoryQuery() = default;
    ISourceInventoryQuery(const ISourceInventoryQuery&) = delete;
    ISourceInventoryQuery& operator=(const ISourceInventoryQuery&) = delete;
    ISourceInventoryQuery(ISourceInventoryQuery&&) = delete;
    ISourceInventoryQuery& operator=(ISourceInventoryQuery&&) = delete;

    [[nodiscard]] virtual base::Result<contracts::SourceInventoryPage>
    list_sources(const contracts::SourceInventoryListRequest& request,
                 base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<ports::SourceInventoryRecord>
    resolve_source(std::string_view source_id, base::CancellationToken cancellation) = 0;
};

// Maps platform inventory into opaque Service source IDs. Desktop never receives device paths.
class SourceInventoryQuery final : public ISourceInventoryQuery {
  public:
    explicit SourceInventoryQuery(ports::ISourceInventory& inventory) noexcept;
    ~SourceInventoryQuery() override = default;

    SourceInventoryQuery(const SourceInventoryQuery&) = delete;
    SourceInventoryQuery& operator=(const SourceInventoryQuery&) = delete;
    SourceInventoryQuery(SourceInventoryQuery&&) = delete;
    SourceInventoryQuery& operator=(SourceInventoryQuery&&) = delete;

    [[nodiscard]] base::Result<contracts::SourceInventoryPage>
    list_sources(const contracts::SourceInventoryListRequest& request,
                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<ports::SourceInventoryRecord>
    resolve_source(std::string_view source_id, base::CancellationToken cancellation) override;

  private:
    ports::ISourceInventory& inventory_;
};

} // namespace aegra::application
