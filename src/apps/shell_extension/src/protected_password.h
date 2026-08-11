#pragma once

#include "secure_password.h"

#include <cstddef>
#include <string_view>
#include <vector>

namespace aegra::shell {

/// Current-user DPAPI protected session credential. No plaintext is retained between calls.
class ProtectedPassword final {
  public:
    ProtectedPassword() = default;
    ~ProtectedPassword();
    ProtectedPassword(const ProtectedPassword&) = delete;
    ProtectedPassword& operator=(const ProtectedPassword&) = delete;
    ProtectedPassword(ProtectedPassword&& other) noexcept;
    ProtectedPassword& operator=(ProtectedPassword&& other) noexcept;

    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] bool protect(std::string_view plaintext);
    [[nodiscard]] bool unprotect(SecurePassword& plaintext) const;

  private:
    std::vector<std::byte> ciphertext_;
};

} // namespace aegra::shell
