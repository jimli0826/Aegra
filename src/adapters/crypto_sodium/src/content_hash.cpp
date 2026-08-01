#include "aegra/adapters/crypto_sodium/content_hash.h"

#include "aegra/base/error.h"

#include <sodium.h>

namespace aegra::adapters::crypto_sodium {
namespace {

[[nodiscard]] const unsigned char* as_unsigned(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<const unsigned char*>(value);
}

[[nodiscard]] unsigned char* as_unsigned(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) C API byte-buffer boundary.
    return reinterpret_cast<unsigned char*>(value);
}

} // namespace

base::Result<Sha256Digest> sha256(const std::span<const std::byte> input) {
    if (sodium_init() < 0) {
        return base::Result<Sha256Digest>::failure(
            {base::ErrorCode::kInternal, "libsodium initialization failed"});
    }
    Sha256Digest result{};
    if (crypto_hash_sha256(as_unsigned(result.data()), as_unsigned(input.data()),
                           static_cast<unsigned long long>(input.size())) != 0) {
        return base::Result<Sha256Digest>::failure(
            {base::ErrorCode::kInternal, "SHA-256 calculation failed"});
    }
    return base::Result<Sha256Digest>::success(result);
}

} // namespace aegra::adapters::crypto_sodium
