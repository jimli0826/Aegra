#include "aegra/adapters/crypto_sodium/secure_string.h"

#include <sodium.h>

namespace aegra::adapters::crypto_sodium {

SecureString::SecureString(const std::string_view value) : value_(value) {}

SecureString::~SecureString() { clear(); }

std::string_view SecureString::view() const noexcept { return value_; }

void SecureString::clear() noexcept {
    if (!value_.empty()) {
        sodium_memzero(value_.data(), value_.size());
        value_.clear();
    }
}

} // namespace aegra::adapters::crypto_sodium
