#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/job.h"

#include <memory>
#include <string_view>

namespace aegra::ports {

class IResolvedSecret {
  public:
    IResolvedSecret() = default;
    virtual ~IResolvedSecret() = default;
    IResolvedSecret(const IResolvedSecret&) = delete;
    IResolvedSecret& operator=(const IResolvedSecret&) = delete;
    IResolvedSecret(IResolvedSecret&&) = delete;
    IResolvedSecret& operator=(IResolvedSecret&&) = delete;

    // The view remains valid until this object is destroyed and must never be logged.
    [[nodiscard]] virtual std::string_view view() const noexcept = 0;
};

class ICredentialResolver {
  public:
    ICredentialResolver() = default;
    virtual ~ICredentialResolver() = default;
    ICredentialResolver(const ICredentialResolver&) = delete;
    ICredentialResolver& operator=(const ICredentialResolver&) = delete;
    ICredentialResolver(ICredentialResolver&&) = delete;
    ICredentialResolver& operator=(ICredentialResolver&&) = delete;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IResolvedSecret>>
    resolve(const contracts::SecretRef& secret_ref,
            const base::CancellationToken& cancellation) = 0;
};

} // namespace aegra::ports
