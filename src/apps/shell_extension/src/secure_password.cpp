#include "pch.h"

#include "secure_password.h"

namespace aegra::shell {

SecurePassword::~SecurePassword() { clear(); }

void SecurePassword::clear() noexcept {
    if (!bytes_.empty()) {
        ::SecureZeroMemory(bytes_.data(), bytes_.size());
        bytes_.clear();
        bytes_.shrink_to_fit();
    }
}

bool SecurePassword::empty() const noexcept { return bytes_.empty(); }

std::string_view SecurePassword::view() const noexcept { return bytes_; }

void SecurePassword::assign_utf8(const std::string_view value) {
    clear();
    bytes_.assign(value.begin(), value.end());
}

bool SecurePassword::assign_from_wide(const std::wstring_view value) {
    clear();
    if (value.empty()) {
        return true;
    }
    const int length = ::WideCharToMultiByte(
        CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (length <= 0) {
        return false;
    }
    bytes_.assign(static_cast<std::size_t>(length), '\0');
    const int written =
        ::WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                              bytes_.data(), length, nullptr, nullptr);
    if (written != length) {
        clear();
        return false;
    }
    return true;
}

} // namespace aegra::shell
