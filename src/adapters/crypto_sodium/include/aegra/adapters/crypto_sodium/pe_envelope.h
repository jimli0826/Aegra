#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::adapters::crypto_sodium {

/// Sizes mirror contracts::kPeEnvelope* wire constants; pe_envelope.cpp static-asserts
/// them against the libsodium algorithm constants.
inline constexpr std::size_t kPeJobKeySize = 32;
inline constexpr std::size_t kPeEnvelopeNonceSize = 24;
inline constexpr std::size_t kPeEnvelopeTagSize = 16;

/// Sealed cross-reboot password envelope (ADR-0026 B): XChaCha20-Poly1305 with the
/// SHA-256 of the canonical job binding as additional data.
struct SealedPeEnvelope final {
    std::array<std::byte, kPeEnvelopeNonceSize> nonce{};
    /// Ciphertext with the 16-byte tag appended (combined AEAD mode).
    std::vector<std::byte> ciphertext_with_tag;
};

/// 256-bit random job key from the CSPRNG. The caller owns zeroization
/// (sodium_memzero) once the key has been persisted or consumed.
[[nodiscard]] base::Result<std::vector<std::byte>> generate_pe_job_key();

/// Seals `password` under `job_key` with AD = SHA-256(`binding`).
/// `binding` is contracts::pe_pending_job_binding(job); any change to a bound job
/// field makes open_pe_password fail authentication.
[[nodiscard]] base::Result<SealedPeEnvelope> seal_pe_password(std::string_view password,
                                                              std::span<const std::byte> job_key,
                                                              std::string_view binding);

/// Opens a sealed envelope. Returns the plaintext password bytes; the caller must
/// zeroize them (sodium_memzero) after handing the password to the archive reader.
[[nodiscard]] base::Result<std::vector<std::byte>>
open_pe_password(std::span<const std::byte> nonce, std::span<const std::byte> ciphertext_with_tag,
                 std::span<const std::byte> job_key, std::string_view binding);

/// Lowercase-hex SHA-256 of the binding preimage, for the informational
/// `binding_digest_hex` job field.
[[nodiscard]] base::Result<std::string> pe_binding_digest_hex(std::string_view binding);

} // namespace aegra::adapters::crypto_sodium
