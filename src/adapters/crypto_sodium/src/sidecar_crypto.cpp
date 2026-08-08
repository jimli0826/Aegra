#include "aegra/adapters/crypto_sodium/sidecar_crypto.h"

#include "aegra/base/error.h"

#include <sodium.h>

#include <array>
#include <string>
#include <utility>

namespace aegra::adapters::crypto_sodium {
namespace {

inline constexpr std::string_view kSidecarContext = "MYBACKUP-V7-SIDECAR";

class SensitiveKey final {
  public:
    ~SensitiveKey() { sodium_memzero(bytes_.data(), bytes_.size()); }
    SensitiveKey() = default;
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

[[nodiscard]] unsigned char* as_unsigned(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<unsigned char*>(value);
}

[[nodiscard]] const unsigned char* as_unsigned(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<const unsigned char*>(value);
}

[[nodiscard]] base::Result<SensitiveKey>
derive_sidecar_key(const std::string_view password, const SidecarProtectionContext& context) {
    SensitiveKey master_key;
    SensitiveKey sidecar_key;
    SensitiveKey pseudo_random_key;
    if (crypto_pwhash(master_key.data(), master_key.size(), password.data(), password.size(),
                      as_unsigned(context.salt.data()), context.kdf.opslimit,
                      static_cast<std::size_t>(context.kdf.memlimit_bytes),
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return base::Result<SensitiveKey>::failure(
            {base::ErrorCode::kInternal, "sidecar password derivation failed"});
    }
    if (crypto_kdf_hkdf_sha256_extract(pseudo_random_key.data(), as_unsigned(context.salt.data()),
                                       context.salt.size(), master_key.data(),
                                       master_key.size()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(sidecar_key.data(), sidecar_key.size(),
                                      kSidecarContext.data(), kSidecarContext.size(),
                                      pseudo_random_key.data()) != 0) {
        return base::Result<SensitiveKey>::failure(
            {base::ErrorCode::kInternal, "sidecar key separation failed"});
    }
    return base::Result<SensitiveKey>::success(std::move(sidecar_key));
}

[[nodiscard]] base::Result<void> validate_request(const std::string_view password,
                                                  const KdfParameters parameters) {
    if (password.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "sidecar password must not be empty"});
    }
    if (sodium_init() < 0) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInternal, "libsodium initialization failed"});
    }
    return validate_kdf_parameters(parameters);
}

} // namespace

base::Result<SidecarProtectionContext>
create_sidecar_protection_context(const KdfParameters parameters,
                                  const std::array<std::byte, kMetadataSaltSize>& salt) {
    auto validation = validate_request("context-validation", parameters);
    if (!validation) {
        return base::Result<SidecarProtectionContext>::failure(validation.error());
    }
    SidecarProtectionContext result;
    result.kdf = parameters;
    result.salt = salt;
    randombytes_buf(result.nonce.data(), result.nonce.size());
    return base::Result<SidecarProtectionContext>::success(result);
}

base::Result<ProtectedSidecarPayload>
protect_sidecar_payload(const std::span<const std::byte> plaintext, const std::string_view password,
                        const std::span<const std::byte> authenticated_data,
                        const SidecarProtectionContext& context) {
    auto validation = validate_request(password, context.kdf);
    if (!validation || plaintext.empty()) {
        return base::Result<ProtectedSidecarPayload>::failure(
            !validation ? validation.error()
                        : base::Error{base::ErrorCode::kInvalidArgument,
                                      "sidecar plaintext must not be empty"});
    }
    auto key = derive_sidecar_key(password, context);
    if (!key) {
        return base::Result<ProtectedSidecarPayload>::failure(key.error());
    }
    ProtectedSidecarPayload result;
    result.nonce = context.nonce;
    result.ciphertext.resize(plaintext.size());
    unsigned long long tag_size = 0;
    const auto status = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        as_unsigned(result.ciphertext.data()), as_unsigned(result.tag.data()), &tag_size,
        as_unsigned(plaintext.data()), plaintext.size(), as_unsigned(authenticated_data.data()),
        authenticated_data.size(), nullptr, as_unsigned(result.nonce.data()), key.value().data());
    if (status != 0 || tag_size != result.tag.size()) {
        return base::Result<ProtectedSidecarPayload>::failure(
            {base::ErrorCode::kInternal, "sidecar encryption failed"});
    }
    return base::Result<ProtectedSidecarPayload>::success(std::move(result));
}

base::Result<std::vector<std::byte>> unprotect_sidecar_payload(
    const std::span<const std::byte> ciphertext, const std::array<std::byte, kMetadataTagSize>& tag,
    const std::string_view password, const std::span<const std::byte> authenticated_data,
    const SidecarProtectionContext& context) {
    auto validation = validate_request(password, context.kdf);
    if (!validation || ciphertext.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            !validation ? validation.error()
                        : base::Error{base::ErrorCode::kInvalidArgument,
                                      "sidecar ciphertext must not be empty"});
    }
    auto key = derive_sidecar_key(password, context);
    if (!key) {
        return base::Result<std::vector<std::byte>>::failure(key.error());
    }
    std::vector<std::byte> plaintext(ciphertext.size());
    const auto status = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
        as_unsigned(plaintext.data()), nullptr, as_unsigned(ciphertext.data()), ciphertext.size(),
        as_unsigned(tag.data()), as_unsigned(authenticated_data.data()), authenticated_data.size(),
        as_unsigned(context.nonce.data()), key.value().data());
    if (status != 0) {
        return base::Result<std::vector<std::byte>>::failure(
            {base::ErrorCode::kUnauthorized, "sidecar authentication failed"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(plaintext));
}

} // namespace aegra::adapters::crypto_sodium
