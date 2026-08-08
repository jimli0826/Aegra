#include "aegra/adapters/crypto_sodium/metadata_crypto.h"

#include "aegra/base/error.h"

#include <sodium.h>

#include <array>
#include <limits>
#include <string>
#include <utility>

namespace aegra::adapters::crypto_sodium {
namespace {

inline constexpr std::uint64_t kMaximumOpslimit = 10;
inline constexpr std::uint64_t kMaximumMemlimitBytes = 1024ULL * 1024ULL * 1024ULL;
inline constexpr std::string_view kMetadataContext = "MYBACKUP-V7-CBOR-METADATA";

class SensitiveKey final {
  public:
    SensitiveKey() = default;
    ~SensitiveKey() { sodium_memzero(bytes_.data(), bytes_.size()); }
    SensitiveKey(const SensitiveKey&) = delete;
    SensitiveKey& operator=(const SensitiveKey&) = delete;
    SensitiveKey(SensitiveKey&& other) noexcept : bytes_(other.bytes_) {
        sodium_memzero(other.bytes_.data(), other.bytes_.size());
    }
    SensitiveKey& operator=(SensitiveKey&& other) noexcept {
        if (this != &other) {
            sodium_memzero(bytes_.data(), bytes_.size());
            bytes_ = other.bytes_;
            sodium_memzero(other.bytes_.data(), other.bytes_.size());
        }
        return *this;
    }

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

[[nodiscard]] base::Result<void> ensure_sodium() {
    static const int initialized = sodium_init();
    if (initialized < 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInternal, "libsodium initialization failed"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<SensitiveKey> derive_metadata_key(std::string_view password,
                                                             std::span<const std::byte> salt,
                                                             const KdfParameters& parameters) {
    SensitiveKey master_key;
    SensitiveKey metadata_key;
    SensitiveKey pseudo_random_key;
    const auto password_size = static_cast<unsigned long long>(password.size());
    const auto* salt_data = as_unsigned(salt.data());
    if (crypto_pwhash(master_key.data(), master_key.size(), &password.front(), password_size,
                      salt_data, parameters.opslimit,
                      static_cast<std::size_t>(parameters.memlimit_bytes),
                      crypto_pwhash_ALG_ARGON2ID13) != 0) {
        return base::Result<SensitiveKey>::failure(
            error(base::ErrorCode::kInternal, "metadata password derivation failed"));
    }
    if (crypto_kdf_hkdf_sha256_extract(pseudo_random_key.data(), salt_data, salt.size(),
                                       master_key.data(), master_key.size()) != 0 ||
        crypto_kdf_hkdf_sha256_expand(metadata_key.data(), metadata_key.size(),
                                      kMetadataContext.data(), kMetadataContext.size(),
                                      pseudo_random_key.data()) != 0) {
        return base::Result<SensitiveKey>::failure(
            error(base::ErrorCode::kInternal, "metadata key separation failed"));
    }
    return base::Result<SensitiveKey>::success(std::move(metadata_key));
}

[[nodiscard]] base::Result<void> validate_request(std::string_view password,
                                                  const KdfParameters& parameters) {
    if (password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "metadata password must not be empty"));
    }
    auto initialized = ensure_sodium();
    if (!initialized) {
        return initialized;
    }
    return validate_kdf_parameters(parameters);
}

} // namespace

KdfParameters default_kdf_parameters() noexcept {
    return {crypto_pwhash_OPSLIMIT_MODERATE, crypto_pwhash_MEMLIMIT_MODERATE,
            kKdfParametersVersion};
}

KdfParameters minimum_kdf_parameters() noexcept {
    return {crypto_pwhash_OPSLIMIT_INTERACTIVE, crypto_pwhash_MEMLIMIT_INTERACTIVE,
            kKdfParametersVersion};
}

base::Result<void> validate_kdf_parameters(const KdfParameters parameters) {
    const auto minimum = minimum_kdf_parameters();
    if (parameters.parameters_version != kKdfParametersVersion) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kUnsupportedVersion, "unsupported metadata KDF parameters"));
    }
    if (parameters.opslimit < minimum.opslimit ||
        parameters.memlimit_bytes < minimum.memlimit_bytes ||
        parameters.opslimit > kMaximumOpslimit ||
        parameters.memlimit_bytes > kMaximumMemlimitBytes) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "metadata KDF parameters are outside policy"));
    }
    return base::Result<void>::success();
}

base::Result<MetadataProtectionContext>
create_metadata_protection_context(const KdfParameters parameters) {
    auto initialized = ensure_sodium();
    if (!initialized) {
        return base::Result<MetadataProtectionContext>::failure(initialized.error());
    }
    auto validation = validate_kdf_parameters(parameters);
    if (!validation) {
        return base::Result<MetadataProtectionContext>::failure(validation.error());
    }
    MetadataProtectionContext result;
    result.kdf = parameters;
    randombytes_buf(result.salt.data(), result.salt.size());
    randombytes_buf(result.nonce.data(), result.nonce.size());
    return base::Result<MetadataProtectionContext>::success(result);
}

base::Result<ProtectedMetadata> protect_metadata(std::span<const std::byte> plaintext,
                                                 std::string_view password,
                                                 std::span<const std::byte> authenticated_data,
                                                 const MetadataProtectionContext& context) {
    auto request = validate_request(password, context.kdf);
    if (!request) {
        return base::Result<ProtectedMetadata>::failure(request.error());
    }
    if (plaintext.empty()) {
        return base::Result<ProtectedMetadata>::failure(
            error(base::ErrorCode::kInvalidArgument, "metadata plaintext must not be empty"));
    }
    ProtectedMetadata result;
    result.kdf = context.kdf;
    result.salt = context.salt;
    result.nonce = context.nonce;
    auto key = derive_metadata_key(password, result.salt, result.kdf);
    if (!key) {
        return base::Result<ProtectedMetadata>::failure(key.error());
    }
    result.ciphertext.resize(plaintext.size());
    unsigned long long tag_size = 0;
    const int status = crypto_aead_xchacha20poly1305_ietf_encrypt_detached(
        as_unsigned(result.ciphertext.data()), as_unsigned(result.tag.data()), &tag_size,
        as_unsigned(plaintext.data()), static_cast<unsigned long long>(plaintext.size()),
        as_unsigned(authenticated_data.data()),
        static_cast<unsigned long long>(authenticated_data.size()), nullptr,
        as_unsigned(result.nonce.data()), key.value().data());
    if (status != 0 || tag_size != result.tag.size()) {
        return base::Result<ProtectedMetadata>::failure(
            error(base::ErrorCode::kInternal, "metadata encryption failed"));
    }
    return base::Result<ProtectedMetadata>::success(std::move(result));
}

base::Result<std::vector<std::byte>>
unprotect_metadata(const ProtectedMetadata& protected_metadata, std::string_view password,
                   std::span<const std::byte> authenticated_data) {
    auto request = validate_request(password, protected_metadata.kdf);
    if (!request) {
        return base::Result<std::vector<std::byte>>::failure(request.error());
    }
    auto key = derive_metadata_key(password, protected_metadata.salt, protected_metadata.kdf);
    if (!key) {
        return base::Result<std::vector<std::byte>>::failure(key.error());
    }
    std::vector<std::byte> plaintext(protected_metadata.ciphertext.size());
    const int status = crypto_aead_xchacha20poly1305_ietf_decrypt_detached(
        as_unsigned(plaintext.data()), nullptr, as_unsigned(protected_metadata.ciphertext.data()),
        static_cast<unsigned long long>(protected_metadata.ciphertext.size()),
        as_unsigned(protected_metadata.tag.data()), as_unsigned(authenticated_data.data()),
        static_cast<unsigned long long>(authenticated_data.size()),
        as_unsigned(protected_metadata.nonce.data()), key.value().data());
    if (status != 0) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kUnauthorized, "metadata authentication failed"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(plaintext));
}

} // namespace aegra::adapters::crypto_sodium
