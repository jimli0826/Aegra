#pragma once

#include "secure_password.h"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <string_view>

namespace aegra::shell {

enum class PasswordDialogResult : std::uint8_t {
    kOk = 1,
    kCancelled = 2,
    kFailed = 3,
};

using PasswordValidator = std::function<HRESULT(std::string_view)>;

/// Modal Shell-owned password dialog (backup ShellExtension pattern).
/// Owner is resolved as GetForegroundWindow / GetActiveWindow; dialog is centered and TOPMOST.
/// Password edit uses ES_PASSWORD (no echo). No global password cache.
[[nodiscard]] PasswordDialogResult prompt_archive_password(std::wstring_view archive_name,
                                                           SecurePassword& password,
                                                           const PasswordValidator& validator);

} // namespace aegra::shell
