#pragma once

#include <string>
#include <string_view>

namespace aegra::shell {

/// Temporary UTF-8 plaintext password. Every owned byte is wiped on clear/destruction.
class SecurePassword final {
  public:
    SecurePassword() = default;
    ~SecurePassword();
    SecurePassword(const SecurePassword&) = delete;
    SecurePassword& operator=(const SecurePassword&) = delete;
    SecurePassword(SecurePassword&&) = delete;
    SecurePassword& operator=(SecurePassword&&) = delete;

    void clear() noexcept;
    [[nodiscard]] bool empty() const noexcept;
    [[nodiscard]] std::string_view view() const noexcept;
    void assign_utf8(std::string_view value);
    [[nodiscard]] bool assign_from_wide(std::wstring_view value);

  private:
    std::string bytes_;
};

} // namespace aegra::shell
