#pragma once

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace aegra::adapters::crypto_sodium {

inline constexpr std::size_t kPayloadNonceSize = 24;
inline constexpr std::size_t kPayloadTagSize = 16;

using PayloadNonce = std::array<std::byte, kPayloadNonceSize>;
using PayloadTag = std::array<std::byte, kPayloadTagSize>;

struct ProtectedPayload final {
    std::vector<std::byte> ciphertext;
    PayloadTag tag{};
};

class PayloadCipher final {
  public:
    ~PayloadCipher();
    PayloadCipher(const PayloadCipher&) = delete;
    PayloadCipher& operator=(const PayloadCipher&) = delete;
    PayloadCipher(PayloadCipher&&) = delete;
    PayloadCipher& operator=(PayloadCipher&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PayloadCipher>>
    create(std::string_view password, KdfParameters parameters,
           const std::array<std::byte, kMetadataSaltSize>& salt);

    /// Same AEAD as create(), but HKDF info = MYBACKUP-V7-FILE-INDEX-PAGE.
    [[nodiscard]] static base::Result<std::unique_ptr<PayloadCipher>>
    create_index_page(std::string_view password, KdfParameters parameters,
                      const std::array<std::byte, kMetadataSaltSize>& salt);

    [[nodiscard]] base::Result<ProtectedPayload>
    protect(std::span<const std::byte> plaintext, std::span<const std::byte> authenticated_data,
            const PayloadNonce& nonce) const;

      /// Encrypts a detached payload in its existing buffer and returns its authentication tag.
      [[nodiscard]] base::Result<PayloadTag>
      protect_in_place(std::span<std::byte> payload,
               std::span<const std::byte> authenticated_data,
               const PayloadNonce& nonce) const;

    [[nodiscard]] base::Result<std::vector<std::byte>>
    unprotect(std::span<const std::byte> ciphertext, std::span<const std::byte> authenticated_data,
              const PayloadNonce& nonce, const PayloadTag& tag) const;

    /// Authenticates and decrypts a detached payload in its existing buffer.
    [[nodiscard]] base::Result<void>
    unprotect_in_place(std::span<std::byte> payload,
               std::span<const std::byte> authenticated_data,
               const PayloadNonce& nonce, const PayloadTag& tag) const;

  private:
    struct Impl;
    explicit PayloadCipher(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

[[nodiscard]] base::Result<PayloadNonce> create_payload_nonce();

} // namespace aegra::adapters::crypto_sodium
