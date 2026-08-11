#include "pch.h"

#include "protected_password.h"

#include <dpapi.h>

#include <cstring>
#include <limits>
#include <utility>

namespace aegra::shell {
namespace {

class LocalDataBlob final {
  public:
    LocalDataBlob() = default;
    ~LocalDataBlob() {
        if (blob.pbData != nullptr) {
            ::SecureZeroMemory(blob.pbData, blob.cbData);
            ::LocalFree(blob.pbData);
        }
    }
    LocalDataBlob(const LocalDataBlob&) = delete;
    LocalDataBlob& operator=(const LocalDataBlob&) = delete;

    DATA_BLOB blob{};
};

[[nodiscard]] bool fits_dword(const std::size_t size) noexcept {
    return size <= static_cast<std::size_t>((std::numeric_limits<DWORD>::max)());
}

} // namespace

ProtectedPassword::~ProtectedPassword() { clear(); }

ProtectedPassword::ProtectedPassword(ProtectedPassword&& other) noexcept
    : ciphertext_(std::move(other.ciphertext_)) {
    other.ciphertext_.clear();
}

ProtectedPassword& ProtectedPassword::operator=(ProtectedPassword&& other) noexcept {
    if (this != &other) {
        clear();
        ciphertext_ = std::move(other.ciphertext_);
        other.ciphertext_.clear();
    }
    return *this;
}

void ProtectedPassword::clear() noexcept {
    if (!ciphertext_.empty()) {
        ::SecureZeroMemory(ciphertext_.data(), ciphertext_.size());
        ciphertext_.clear();
        ciphertext_.shrink_to_fit();
    }
}

bool ProtectedPassword::empty() const noexcept { return ciphertext_.empty(); }

bool ProtectedPassword::protect(const std::string_view plaintext) {
    clear();
    if (plaintext.empty() || !fits_dword(plaintext.size())) {
        return plaintext.empty();
    }
    DATA_BLOB input{static_cast<DWORD>(plaintext.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(plaintext.data()))};
    LocalDataBlob output;
    if (!::CryptProtectData(&input, L"Aegra Explorer archive password", nullptr, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output.blob)) {
        return false;
    }
    ciphertext_.resize(output.blob.cbData);
    std::memcpy(ciphertext_.data(), output.blob.pbData, output.blob.cbData);
    return true;
}

bool ProtectedPassword::unprotect(SecurePassword& plaintext) const {
    plaintext.clear();
    if (ciphertext_.empty() || !fits_dword(ciphertext_.size())) {
        return false;
    }
    DATA_BLOB input{static_cast<DWORD>(ciphertext_.size()),
                    reinterpret_cast<BYTE*>(const_cast<std::byte*>(ciphertext_.data()))};
    LocalDataBlob output;
    if (!::CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                              &output.blob)) {
        return false;
    }
    plaintext.assign_utf8(
        std::string_view(reinterpret_cast<const char*>(output.blob.pbData), output.blob.cbData));
    return true;
}

} // namespace aegra::shell
