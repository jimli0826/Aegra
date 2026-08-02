#pragma once

#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/random.h"

namespace aegra::adapters::windows_system {

class WindowsSystemClock final : public ports::IClock {
  public:
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override;
};

class WindowsCryptographicRandom final : public ports::IRandomSource {
  public:
    [[nodiscard]] base::Result<void> fill(std::span<std::byte> destination,
                                          const base::CancellationToken& cancellation) override;
};

class WindowsCredentialResolver final : public ports::ICredentialResolver {
  public:
    // Resolves only wincred://<target> references. The generic credential blob is copied into
    // locked memory and zeroed before release.
    [[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
    resolve(const contracts::SecretRef& secret_ref,
            const base::CancellationToken& cancellation) override;
};

} // namespace aegra::adapters::windows_system
