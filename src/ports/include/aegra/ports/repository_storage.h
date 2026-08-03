#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/object_storage.h"

#include <memory>
#include <string_view>

namespace aegra::ports {

// Opened Repository root access for control-plane catalog/test operations.
// Does not parse Archive payloads or own durable credentials.
class IRepositoryStorageAccess {
  public:
    IRepositoryStorageAccess() = default;
    virtual ~IRepositoryStorageAccess() = default;
    IRepositoryStorageAccess(const IRepositoryStorageAccess&) = delete;
    IRepositoryStorageAccess& operator=(const IRepositoryStorageAccess&) = delete;
    IRepositoryStorageAccess(IRepositoryStorageAccess&&) = delete;
    IRepositoryStorageAccess& operator=(IRepositoryStorageAccess&&) = delete;

    [[nodiscard]] virtual IObjectReader& reader() noexcept = 0;
    [[nodiscard]] virtual IPrefixEnumerator& enumerator() noexcept = 0;
};

class IRepositoryStorageFactory {
  public:
    IRepositoryStorageFactory() = default;
    virtual ~IRepositoryStorageFactory() = default;
    IRepositoryStorageFactory(const IRepositoryStorageFactory&) = delete;
    IRepositoryStorageFactory& operator=(const IRepositoryStorageFactory&) = delete;
    IRepositoryStorageFactory(IRepositoryStorageFactory&&) = delete;
    IRepositoryStorageFactory& operator=(IRepositoryStorageFactory&&) = delete;

    // Opens the Repository root identified by the control-plane locator.
    // Offline, missing, or unauthorized roots return stable ErrorCode failures.
    [[nodiscard]] virtual base::Result<std::unique_ptr<IRepositoryStorageAccess>>
    open(std::string_view locator, base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
