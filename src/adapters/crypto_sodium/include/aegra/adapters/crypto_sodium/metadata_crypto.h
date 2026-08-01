#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace aegra::adapters::crypto_sodium {

inline constexpr std::size_t kMetadataSaltSize = 32;
inline constexpr std::size_t kMetadataNonceSize = 24;
inline constexpr std::size_t kMetadataTagSize = 16;
inline constexpr std::uint32_t kKdfParametersVersion = 1;

struct KdfParameters final {
    std::uint64_t opslimit{0};
    std::uint64_t memlimit_bytes{0};
    std::uint32_t parameters_version{kKdfParametersVersion};
};

struct MetadataProtectionContext final {
    KdfParameters kdf;
    std::array<std::byte, kMetadataSaltSize> salt{};
    std::array<std::byte, kMetadataNonceSize> nonce{};
};

struct ProtectedMetadata final {
    KdfParameters kdf;
    std::array<std::byte, kMetadataSaltSize> salt{};
    std::array<std::byte, kMetadataNonceSize> nonce{};
    std::vector<std::byte> ciphertext;
    std::array<std::byte, kMetadataTagSize> tag{};
};

[[nodiscard]] KdfParameters default_kdf_parameters() noexcept;
[[nodiscard]] KdfParameters minimum_kdf_parameters() noexcept;
[[nodiscard]] base::Result<void> validate_kdf_parameters(KdfParameters parameters);
[[nodiscard]] base::Result<MetadataProtectionContext>
create_metadata_protection_context(KdfParameters parameters);

[[nodiscard]] base::Result<ProtectedMetadata>
protect_metadata(std::span<const std::byte> plaintext, std::string_view password,
                 std::span<const std::byte> authenticated_data,
                 const MetadataProtectionContext& context);

[[nodiscard]] base::Result<std::vector<std::byte>>
unprotect_metadata(const ProtectedMetadata& protected_metadata, std::string_view password,
                   std::span<const std::byte> authenticated_data);

} // namespace aegra::adapters::crypto_sodium
