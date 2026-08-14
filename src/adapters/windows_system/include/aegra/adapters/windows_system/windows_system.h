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
    // Resolves only dpapi-lm:<entropy_id>:<base64>. Ciphertext is DPAPI-protected with
    // CRYPTPROTECT_LOCAL_MACHINE and pOptionalEntropy = UTF-8(entropy_id) (schedule_id for
    // schedules). Plaintext is copied into locked memory and zeroed on release.
    [[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
    resolve(const contracts::SecretRef& secret_ref,
            const base::CancellationToken& cancellation) override;
};

/// Protects secret material with DPAPI (CRYPTPROTECT_LOCAL_MACHINE).
/// pOptionalEntropy is the UTF-8 bytes of entropy_id (use schedule_id for schedule passwords;
/// job_id for one-shot backups). Returns SecretRef dpapi-lm:<entropy_id>:<base64>.
/// Never log the result value. Maximum secret material is 32 bytes.
[[nodiscard]] base::Result<contracts::SecretRef>
protect_local_machine_secret(std::string_view secret_material, std::string_view entropy_id);

/// Same as protect_local_machine_secret but allows up to 1024 bytes (network share auth pack).
[[nodiscard]] base::Result<contracts::SecretRef>
protect_local_machine_secret_blob(std::string_view secret_material, std::string_view entropy_id);

} // namespace aegra::adapters::windows_system
