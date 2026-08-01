#pragma once

#include <string>
#include <string_view>

namespace aegra::adapters::crypto_sodium {

class SecureString final {
  public:
    explicit SecureString(std::string_view value);
    ~SecureString();
    SecureString(const SecureString&) = delete;
    SecureString& operator=(const SecureString&) = delete;
    SecureString(SecureString&&) = delete;
    SecureString& operator=(SecureString&&) = delete;

    [[nodiscard]] std::string_view view() const noexcept;
    void clear() noexcept;

  private:
    std::string value_;
};

} // namespace aegra::adapters::crypto_sodium
