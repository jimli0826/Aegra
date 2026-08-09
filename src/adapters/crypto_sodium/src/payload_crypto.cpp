#include "aegra/adapters/crypto_sodium/payload_crypto.h"

#include "aegra/base/error.h"

#include <sodium.h>

#include <array>
#include <string>
#include <utility>

namespace aegra::adapters::crypto_sodium {
namespace {

inline constexpr std::string_view kPayloadContext = "MYBACKUP-V7-CHUNK-PAYLOAD";
inline constexpr std::string_view kIndexPageContext = "MYBACKUP-V7-FILE-INDEX-PAGE";

class SensitiveKey final {
  public:
    SensitiveKey() = default;
    ~SensitiveKey() { sodium_memzero(bytes_.data(), bytes_.size()); }
    SensitiveKey(const SensitiveKey&) = delete;
    SensitiveKey& operator=(const SensitiveKey&) = delete;
    SensitiveKey(SensitiveKey&& other) noexcept : bytes_(other.bytes_) {
        sodium_memzero(other.bytes_.data(), other.bytes_.size());
    }
    SensitiveKey& operator=(SensitiveKey&&) = delete;

    [[nodiscard]] unsigned char* data() noexcept { return bytes_.data(); }
    [[nodiscard]] const unsigned char* data() const noexcept { return bytes_.data(); }
    [[nodiscard]] std::size_t size() const noexcept { return bytes_.size(); }

  private:
    std::array<unsigned char, crypto_aead_xchacha20poly1305_ietf_KEYBYTES> bytes_{};
};

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] unsigned char* as_unsigned(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<unsigned char*>(value);
}

[[nodiscard]] const unsigned char* as_unsigned(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<const unsigned char*>(value);
}

[[nodiscard]] base::Result<void> validate_request(const std::string_view password,
                                                  const KdfParameters parameters) {
    if (password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "payload password must not be empty"));
    }
    if (sodium_init() < 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInternal, "libsodium initialization failed"));
    }
    return validate_kdf_parameters(parameters);
}

[[nodiscard]] base::Result<SensitiveKey>
derive_separated_key(const std::string_view password, const KdfParameters parameters,
                     const std::array<std::byte, kMetadataSaltSize>& salt,
                     const std::string_view context) {
    SensitiveKey master_key;
    SensitiveKey derived_key;
    SensitiveKey pseudo_random_key;
    if (crypto_pwhash(master_key.data(), master_key.size(), password.data(), password.size(),
                      as_unsigned(salt.data()), parameters.opslimit,
                      static_cast<std::size_t>(parameters.memlimit_bytes),
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return base::Result<SensitiveKey>::failure(
            error(base::ErrorCode::kInternal, "payload password derivation failed"));
    }
    if (crypto_kdf_hkdf_sha256_extract(pseudo_random_key.data(), as_unsigned(salt.data()),
                                       salt.size(), master_key.data(), master_key.size()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(derived_key.data(), derived_key.size(), context.data(),
                                      context.size(), pseudo_random_key.data()) != 0) {
        return base::Result<SensitiveKey>::failure(
            error(base::ErrorCode::kInternal, "payload key separation failed"));
    }
    return base::Result<SensitiveKey>::success(std::move(derived_key));
}

} // namespace

struct PayloadCipher::Impl final {
    explicit Impl(SensitiveKey derived_key) : key(std::move(derived_key)) {}
    SensitiveKey key;
};

PayloadCipher::PayloadCipher(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PayloadCipher::~PayloadCipher() = default;

base::Result<std::unique_ptr<PayloadCipher>>
PayloadCipher::create(const std::string_view password, const KdfParameters parameters,
                      const std::array<std::byte, kMetadataSaltSize>& salt) {
    auto validation = validate_request(password, parameters);
    if (!validation) {
        return base::Result<std::unique_ptr<PayloadCipher>>::failure(validation.error());
    }
    auto key = derive_separated_key(password, parameters, salt, kPayloadContext);
    if (!key) {
        return base::Result<std::unique_ptr<PayloadCipher>>::failure(key.error());
    }
    auto implementation = std::make_unique<Impl>(std::move(key).value());
    return base::Result<std::unique_ptr<PayloadCipher>>::success(
        std::unique_ptr<PayloadCipher>(new PayloadCipher(std::move(implementation))));
}

base::Result<std::unique_ptr<PayloadCipher>>
PayloadCipher::create_index_page(const std::string_view password, const KdfParameters parameters,
                                 const std::array<std::byte, kMetadataSaltSize>& salt) {
    auto validation = validate_request(password, parameters);
    if (!validation) {
        return base::Result<std::unique_ptr<PayloadCipher>>::failure(validation.error());
    }
    auto key = derive_separated_key(password, parameters, salt, kIndexPageContext);
    if (!key) {
        return base::Result<std::unique_ptr<PayloadCipher>>::failure(key.error());
    }
    auto implementation = std::make_unique<Impl>(std::move(key).value());
    return base::Result<std::unique_ptr<PayloadCipher>>::success(
        std::unique_ptr<PayloadCipher>(new PayloadCipher(std::move(implementation))));
}

base::Result<ProtectedPayload>
PayloadCipher::protect(const std::span<const std::byte> plaintext,
                       const std::span<const std::byte> authenticated_data,
                       const PayloadNonce& nonce) const {
    ProtectedPayload result;
    result.ciphertext.resize(plaintext.size());
    std::byte empty_output{};
    const auto output = result.ciphertext.empty() ? as_unsigned(&empty_output)
                                                  : as_unsigned(result.ciphertext.data());
    const auto input =
        plaintext.empty() ? as_unsigned(&empty_output) : as_unsigned(plaintext.data());
    unsigned long long tag_size = 0;
    const auto status = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        output, as_unsigned(result.tag.data()), &tag_size, input, plaintext.size(),
        as_unsigned(authenticated_data.data()), authenticated_data.size(), nullptr,
        as_unsigned(nonce.data()), implementation_->key.data());
    if (status != 0 || tag_size != result.tag.size()) {
        return base::Result<ProtectedPayload>::failure(
            error(base::ErrorCode::kInternal, "payload encryption failed"));
    }
    return base::Result<ProtectedPayload>::success(std::move(result));
}

base::Result<PayloadTag>
PayloadCipher::protect_in_place(const std::span<std::byte> payload,
                                const std::span<const std::byte> authenticated_data,
                                const PayloadNonce& nonce) const {
    PayloadTag tag{};
    std::byte empty_payload{};
    const auto data = payload.empty() ? as_unsigned(&empty_payload)
                                      : as_unsigned(payload.data());
    unsigned long long tag_size = 0;
    const auto status = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        data, as_unsigned(tag.data()), &tag_size, data, payload.size(),
        as_unsigned(authenticated_data.data()), authenticated_data.size(), nullptr,
        as_unsigned(nonce.data()), implementation_->key.data());
    if (status != 0 || tag_size != tag.size()) {
        return base::Result<PayloadTag>::failure(
            error(base::ErrorCode::kInternal, "payload encryption failed"));
    }
    return base::Result<PayloadTag>::success(tag);
}

base::Result<std::vector<std::byte>>
PayloadCipher::unprotect(const std::span<const std::byte> ciphertext,
                         const std::span<const std::byte> authenticated_data,
                         const PayloadNonce& nonce, const PayloadTag& tag) const {
    std::vector<std::byte> plaintext(ciphertext.size());
    std::byte empty_output{};
    const auto output =
        plaintext.empty() ? as_unsigned(&empty_output) : as_unsigned(plaintext.data());
    const auto input =
        ciphertext.empty() ? as_unsigned(&empty_output) : as_unsigned(ciphertext.data());
    const auto status = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
        output, nullptr, input, ciphertext.size(), as_unsigned(tag.data()),
        as_unsigned(authenticated_data.data()), authenticated_data.size(),
        as_unsigned(nonce.data()), implementation_->key.data());
    if (status != 0) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kUnauthorized, "payload authentication failed"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(plaintext));
}

base::Result<void>
PayloadCipher::unprotect_in_place(const std::span<std::byte> payload,
                                  const std::span<const std::byte> authenticated_data,
                                  const PayloadNonce& nonce, const PayloadTag& tag) const {
    std::byte empty_payload{};
    const auto data = payload.empty() ? as_unsigned(&empty_payload) : as_unsigned(payload.data());
    const auto status = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
        data, nullptr, data, payload.size(), as_unsigned(tag.data()),
        as_unsigned(authenticated_data.data()), authenticated_data.size(),
        as_unsigned(nonce.data()), implementation_->key.data());
    if (status != 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kUnauthorized, "payload authentication failed"));
    }
    return base::Result<void>::success();
}

base::Result<PayloadNonce> create_payload_nonce() {
    if (sodium_init() < 0) {
        return base::Result<PayloadNonce>::failure(
            error(base::ErrorCode::kInternal, "libsodium initialization failed"));
    }
    PayloadNonce nonce{};
    do {
        randombytes_buf(nonce.data(), nonce.size());
    } while (sodium_is_zero(as_unsigned(nonce.data()), nonce.size()) != 0);
    return base::Result<PayloadNonce>::success(nonce);
}

} // namespace aegra::adapters::crypto_sodium
