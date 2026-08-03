#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/repository_query.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/ports/object_storage.h"

#include <memory>

namespace aegra::application {

class IPersonalRepositoryQuery {
  public:
    IPersonalRepositoryQuery() = default;
    virtual ~IPersonalRepositoryQuery() = default;
    IPersonalRepositoryQuery(const IPersonalRepositoryQuery&) = delete;
    IPersonalRepositoryQuery& operator=(const IPersonalRepositoryQuery&) = delete;
    IPersonalRepositoryQuery(IPersonalRepositoryQuery&&) = delete;
    IPersonalRepositoryQuery& operator=(IPersonalRepositoryQuery&&) = delete;

    [[nodiscard]] virtual base::Result<contracts::RecoveryPointPage>
    list_recovery_points(const contracts::RecoveryPointListRequest& request,
                         base::CancellationToken cancellation) = 0;
};

class PersonalRepositoryQuery final : public IPersonalRepositoryQuery {
  public:
    PersonalRepositoryQuery() noexcept;
    PersonalRepositoryQuery(ports::IObjectReader& reader, ports::IPrefixEnumerator& enumerator,
                            personal_repository::CatalogScannerLimits limits = {});
    ~PersonalRepositoryQuery() override;

    PersonalRepositoryQuery(const PersonalRepositoryQuery&) = delete;
    PersonalRepositoryQuery& operator=(const PersonalRepositoryQuery&) = delete;
    PersonalRepositoryQuery(PersonalRepositoryQuery&&) = delete;
    PersonalRepositoryQuery& operator=(PersonalRepositoryQuery&&) = delete;

    [[nodiscard]] base::Result<contracts::RecoveryPointPage>
    list_recovery_points(const contracts::RecoveryPointListRequest& request,
                         base::CancellationToken cancellation) override;

  private:
    std::unique_ptr<personal_repository::RepositoryCatalogScanner> scanner_;
};

} // namespace aegra::application
