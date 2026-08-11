#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::shell {

/// Load a Shell Extension UI string for the current user UI language
/// (GetUserDefaultUILanguage: en / zh-CN / zh-TW / ja / de; fallback English).
[[nodiscard]] std::wstring load_shell_string(unsigned id);

/// Replace the first "%1" placeholder with @p arg1.
[[nodiscard]] std::wstring format_shell_string(unsigned id, std::wstring_view arg1);

/// Format a string whose "%1" placeholder is an unsigned decimal value.
[[nodiscard]] std::wstring format_shell_string_u32(unsigned id, std::uint32_t value);

} // namespace aegra::shell
