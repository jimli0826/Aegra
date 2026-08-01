#pragma once

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

namespace aegra::adapters::crypto_sodium {

struct SidecarProtectionContext final {
    KdfParameters kdf;
    std::array<std::byte, kMetadataSaltSize> salt{};
    std::array<std::byte, kMetadataNonceSize> nonce{};
};

struct ProtectedSidecarPayload final {
    std::array<std::byte, kMetadataNonceSize> nonce{};
    std::vector<std::byte> ciphertext;
    std::array<std::byte, kMetadataTagSize> tag{};
};

[[nodiscard]] base::Result<SidecarProtectionContext>
create_sidecar_protection_context(KdfParameters parameters,
                                  const std::array<std::byte, kMetadataSaltSize>& salt);

[[nodiscard]] base::Result<ProtectedSidecarPayload>
protect_sidecar_payload(std::span<const std::byte> plaintext, std::string_view password,
                        std::span<const std::byte> authenticated_data,
                        const SidecarProtectionContext& context);

[[nodiscard]] base::Result<std::vector<std::byte>>
unprotect_sidecar_payload(std::span<const std::byte> ciphertext,
                          const std::array<std::byte, kMetadataTagSize>& tag,
                          std::string_view password, std::span<const std::byte> authenticated_data,
                          const SidecarProtectionContext& context);

} // namespace aegra::adapters::crypto_sodium
