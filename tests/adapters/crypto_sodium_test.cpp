#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/crypto_sodium/secure_string.h"
#include "aegra/adapters/crypto_sodium/sidecar_crypto.h"

#include "aegra/base/error.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <vector>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::vector<std::byte> bytes(std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char value : text) {
        result.push_back(static_cast<std::byte>(value));
    }
    return result;
}

bool test_authenticated_roundtrip() {
    const auto plaintext = bytes("string-key CBOR metadata");
    const auto aad = bytes("backup header and envelope");
    const auto context = aegra::adapters::crypto_sodium::create_metadata_protection_context(
        aegra::adapters::crypto_sodium::minimum_kdf_parameters());
    if (!context) {
        return false;
    }
    const auto protected_metadata = aegra::adapters::crypto_sodium::protect_metadata(
        plaintext, "correct horse battery staple", aad, context.value());
    bool passed = expect(protected_metadata.has_value(), "metadata encryption succeeds");
    if (!protected_metadata) {
        return false;
    }
    passed &= expect(protected_metadata.value().ciphertext != plaintext,
                     "metadata is not stored as plaintext");
    const auto decrypted = aegra::adapters::crypto_sodium::unprotect_metadata(
        protected_metadata.value(), "correct horse battery staple", aad);
    passed &= expect(decrypted.has_value(), "metadata authentication succeeds");
    passed &= expect(decrypted.has_value() && decrypted.value() == plaintext,
                     "metadata survives encrypted roundtrip");
    return passed;
}

bool test_authentication_failures() {
    const auto plaintext = bytes("private metadata");
    const auto aad = bytes("authenticated header");
    const auto context = aegra::adapters::crypto_sodium::create_metadata_protection_context(
        aegra::adapters::crypto_sodium::minimum_kdf_parameters());
    if (!context) {
        return false;
    }
    auto protected_metadata = aegra::adapters::crypto_sodium::protect_metadata(
        plaintext, "password", aad, context.value());
    if (!protected_metadata) {
        return false;
    }
    protected_metadata.value().tag.front() ^= std::byte{0x01};
    const auto tampered = aegra::adapters::crypto_sodium::unprotect_metadata(
        protected_metadata.value(), "password", aad);
    bool passed = expect(!tampered.has_value(), "tampered tag is rejected");
    passed &= expect(tampered.error().code == aegra::base::ErrorCode::kUnauthorized,
                     "authentication failure has stable error code");
    return passed;
}

bool test_sidecar_crypto_and_hash() {
    namespace crypto = aegra::adapters::crypto_sodium;
    const auto plaintext = bytes("sidecar block hashes");
    const auto aad = bytes("authenticated sidecar header");
    const auto metadata_context =
        crypto::create_metadata_protection_context(crypto::minimum_kdf_parameters());
    if (!metadata_context) {
        return false;
    }
    const auto context = crypto::create_sidecar_protection_context(metadata_context.value().kdf,
                                                                   metadata_context.value().salt);
    if (!context) {
        return false;
    }
    const auto protected_payload =
        crypto::protect_sidecar_payload(plaintext, "password", aad, context.value());
    bool passed = expect(protected_payload.has_value(), "sidecar encryption succeeds");
    if (!protected_payload) {
        return false;
    }
    const auto decrypted = crypto::unprotect_sidecar_payload(protected_payload.value().ciphertext,
                                                             protected_payload.value().tag,
                                                             "password", aad, context.value());
    passed &= expect(decrypted.has_value() && decrypted.value() == plaintext,
                     "sidecar survives encrypted roundtrip");
    const auto digest = crypto::sha256(bytes("abc"));
    passed &= expect(digest.has_value() && digest.value().front() == std::byte{0xBA} &&
                         digest.value().back() == std::byte{0xAD},
                     "SHA-256 digest matches the known abc vector");
    return passed;
}

bool test_secure_string_lifecycle() {
    aegra::adapters::crypto_sodium::SecureString secret("temporary-password");
    bool passed = expect(secret.view() == "temporary-password", "secure string exposes a view");
    secret.clear();
    passed &= expect(secret.view().empty(), "secure string clears its logical value");
    return passed;
}

int run_tests() {
    return test_authenticated_roundtrip() && test_authentication_failures() &&
                   test_sidecar_crypto_and_hash() && test_secure_string_lifecycle()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
