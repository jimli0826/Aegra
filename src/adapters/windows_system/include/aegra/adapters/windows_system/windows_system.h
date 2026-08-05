#pragma once

#include "aegra/contracts/job.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/credential.h"
#include "aegra/ports/random.h"

#include <string_view>

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

/// Stores a generic Windows credential as wincred://<target_name> and returns that SecretRef.
/// Overwrites an existing target. Used for one-shot personal archive passwords.
[[nodiscard]] base::Result<contracts::SecretRef>
store_generic_windows_credential(std::string_view target_name, std::string_view secret_material);

} // namespace aegra::adapters::windows_system
