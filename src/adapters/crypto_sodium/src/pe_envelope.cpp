#include "aegra/adapters/crypto_sodium/pe_envelope.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"

#include <sodium.h>

#include <limits>

namespace aegra::adapters::crypto_sodium {
namespace {

static_assert(kPeJobKeySize == crypto_aead_xchacha20poly1305_ietf_KEYBYTES);
static_assert(kPeEnvelopeNonceSize == crypto_aead_xchacha20poly1305_ietf_NPUBBYTES);
static_assert(kPeEnvelopeTagSize == crypto_aead_xchacha20poly1305_ietf_ABYTES);

inline constexpr std::size_t kMaximumPasswordSize = 4096 - kPeEnvelopeTagSize;

[[nodiscard]] unsigned char* as_unsigned(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<unsigned char*>(value);
}

[[nodiscard]] const unsigned char* as_unsigned(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<const unsigned char*>(value);
}

[[nodiscard]] const unsigned char* as_unsigned(const char* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<const unsigned char*>(value);
}

[[nodiscard]] base::Result<void> require_sodium() {
    if (sodium_init() < 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInternal, "libsodium initialization failed"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<Sha256Digest> binding_digest(const std::string_view binding) {
    if (binding.empty()) {
        return base::Result<Sha256Digest>::failure(
            {base::ErrorCode::kInvalidArgument, "pe envelope binding must not be empty"});
    }
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) byte view of UTF-8 text.
    const auto* bytes = reinterpret_cast<const std::byte*>(binding.data());
    return sha256(std::span<const std::byte>(bytes, binding.size()));
}

[[nodiscard]] base::Result<void> validate_job_key(const std::span<const std::byte> job_key) {
    if (job_key.size() != kPeJobKeySize) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "pe job key size is invalid"});
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<std::vector<std::byte>> generate_pe_job_key() {
    if (auto ready = require_sodium(); !ready) {
        return base::Result<std::vector<std::byte>>::failure(ready.error());
    }
    std::vector<std::byte> key(kPeJobKeySize);
    randombytes_buf(key.data(), key.size());
    return base::Result<std::vector<std::byte>>::success(std::move(key));
}

base::Result<SealedPeEnvelope> seal_pe_password(const std::string_view password,
                                                const std::span<const std::byte> job_key,
                                                const std::string_view binding) {
    if (auto ready = require_sodium(); !ready) {
        return base::Result<SealedPeEnvelope>::failure(ready.error());
    }
    if (password.empty() || password.size() > kMaximumPasswordSize) {
        return base::Result<SealedPeEnvelope>::failure(
            {base::ErrorCode::kInvalidArgument, "pe envelope password size is invalid"});
    }
    if (auto valid = validate_job_key(job_key); !valid) {
        return base::Result<SealedPeEnvelope>::failure(valid.error());
    }
    auto digest = binding_digest(binding);
    if (!digest) {
        return base::Result<SealedPeEnvelope>::failure(digest.error());
    }
    SealedPeEnvelope envelope;
    randombytes_buf(envelope.nonce.data(), envelope.nonce.size());
    envelope.ciphertext_with_tag.resize(password.size() + kPeEnvelopeTagSize);
    unsigned long long ciphertext_size = 0;
    const auto status = crypto_aead_xchacha20poly1305_ietf_encrypt(
        as_unsigned(envelope.ciphertext_with_tag.data()), &ciphertext_size,
        as_unsigned(password.data()), password.size(), as_unsigned(digest.value().data()),
        digest.value().size(), nullptr, as_unsigned(envelope.nonce.data()),
        as_unsigned(job_key.data()));
    if (status != 0 || ciphertext_size != envelope.ciphertext_with_tag.size()) {
        return base::Result<SealedPeEnvelope>::failure(
            {base::ErrorCode::kInternal, "pe envelope encryption failed"});
    }
    return base::Result<SealedPeEnvelope>::success(std::move(envelope));
}

base::Result<std::vector<std::byte>> open_pe_password(
    const std::span<const std::byte> nonce, const std::span<const std::byte> ciphertext_with_tag,
    const std::span<const std::byte> job_key, const std::string_view binding) {
    if (auto ready = require_sodium(); !ready) {
        return base::Result<std::vector<std::byte>>::failure(ready.error());
    }
    if (nonce.size() != kPeEnvelopeNonceSize ||
        ciphertext_with_tag.size() <= kPeEnvelopeTagSize ||
        ciphertext_with_tag.size() > kMaximumPasswordSize + kPeEnvelopeTagSize) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kInvalidArgument, "pe envelope payload shape is invalid"});
    }
    if (auto valid = validate_job_key(job_key); !valid) {
        return base::Result<std::vector<std::byte>>::failure(valid.error());
    }
    auto digest = binding_digest(binding);
    if (!digest) {
        return base::Result<std::vector<std::byte>>::failure(digest.error());
    }
    std::vector<std::byte> plaintext(ciphertext_with_tag.size() - kPeEnvelopeTagSize);
    unsigned long long plaintext_size = 0;
    const auto status = crypto_aead_xchacha20poly1305_ietf_decrypt(
        as_unsigned(plaintext.data()), &plaintext_size, nullptr,
        as_unsigned(ciphertext_with_tag.data()), ciphertext_with_tag.size(),
        as_unsigned(digest.value().data()), digest.value().size(), as_unsigned(nonce.data()),
        as_unsigned(job_key.data()));
    if (status != 0 || plaintext_size != plaintext.size()) {
        sodium_memzero(plaintext.data(), plaintext.size());
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kUnauthorized, "pe envelope authentication failed"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(plaintext));
}

base::Result<std::string> pe_binding_digest_hex(const std::string_view binding) {
    if (auto ready = require_sodium(); !ready) {
        return base::Result<std::string>::failure(ready.error());
    }
    auto digest = binding_digest(binding);
    if (!digest) {
        return base::Result<std::string>::failure(digest.error());
    }
    std::string hex(digest.value().size() * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), as_unsigned(digest.value().data()),
                   digest.value().size());
    hex.resize(digest.value().size() * 2);
    return base::Result<std::string>::success(std::move(hex));
}

} // namespace aegra::adapters::crypto_sodium
