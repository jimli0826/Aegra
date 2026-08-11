#include "pch.h"

#include "password_dialog.h"
#include "resource.h"
#include "shell_strings.h"

namespace aegra::shell {
namespace {

struct PasswordDialogState final {
    std::wstring_view archive_name;
    SecurePassword* password{nullptr};
    const PasswordValidator* validator{nullptr};
};

constexpr INT_PTR kValidationFailed = 3;

void wipe_wide(std::wstring& value) noexcept {
    if (!value.empty()) {
        ::SecureZeroMemory(value.data(), value.size() * sizeof(wchar_t));
        value.clear();
        value.shrink_to_fit();
    }
}

void center_and_raise(HWND dialog) noexcept {
    RECT dialog_rect{};
    RECT work_area{};
    if (!::GetWindowRect(dialog, &dialog_rect) ||
        !::SystemParametersInfoW(SPI_GETWORKAREA, 0, &work_area, 0)) {
        ::SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
        return;
    }
    const int width = dialog_rect.right - dialog_rect.left;
    const int height = dialog_rect.bottom - dialog_rect.top;
    const int x = work_area.left + ((work_area.right - work_area.left) - width) / 2;
    const int y = work_area.top + ((work_area.bottom - work_area.top) - height) / 2;
    // Match backup ShellExtension: center on work area and pin above Explorer windows.
    ::SetWindowPos(dialog, HWND_TOPMOST, x, y, 0, 0, SWP_NOSIZE | SWP_SHOWWINDOW);
    ::BringWindowToTop(dialog);
    ::SetForegroundWindow(dialog);
}

void apply_localized_dialog_text(HWND dialog, const std::wstring_view archive_name) {
    if (!archive_name.empty()) {
        ::SetWindowTextW(dialog, format_shell_string(IDS_PASSWORD_CAPTION, archive_name).c_str());
    } else {
        ::SetWindowTextW(dialog, load_shell_string(IDS_PASSWORD_CAPTION_DEFAULT).c_str());
    }
    ::SetDlgItemTextW(dialog, IDC_PASSWORD_PROMPT, load_shell_string(IDS_PASSWORD_PROMPT).c_str());
    ::SetDlgItemTextW(dialog, IDC_PASSWORD_ERROR, L"");
    ::SetDlgItemTextW(dialog, IDOK, load_shell_string(IDS_PASSWORD_OK).c_str());
    ::SetDlgItemTextW(dialog, IDCANCEL, load_shell_string(IDS_PASSWORD_CANCEL).c_str());
}

void show_password_error(HWND dialog) {
    // Left of the OK button; red text via WM_CTLCOLORSTATIC.
    ::SetDlgItemTextW(dialog, IDC_PASSWORD_ERROR, load_shell_string(IDS_PASSWORD_INCORRECT).c_str());
    const HWND error = ::GetDlgItem(dialog, IDC_PASSWORD_ERROR);
    if (error != nullptr) {
        ::ShowWindow(error, SW_SHOW);
        ::InvalidateRect(error, nullptr, TRUE);
    }
}

INT_PTR CALLBACK password_dialog_proc(HWND dialog, UINT message, WPARAM wparam, LPARAM lparam) {
    switch (message) {
    case WM_INITDIALOG: {
        auto* state = reinterpret_cast<PasswordDialogState*>(lparam);
        ::SetWindowLongPtrW(dialog, DWLP_USER, reinterpret_cast<LONG_PTR>(state));
        apply_localized_dialog_text(dialog, state != nullptr ? state->archive_name : L"");
        center_and_raise(dialog);
        const HWND edit = ::GetDlgItem(dialog, IDC_PASSWORD_EDIT);
        if (edit != nullptr) {
            ::SendMessageW(edit, EM_SETLIMITTEXT, 512, 0);
            ::SetFocus(edit);
            return FALSE;
        }
        return TRUE;
    }
    case WM_CTLCOLORSTATIC: {
        const auto control = reinterpret_cast<HWND>(lparam);
        if (control != nullptr && ::GetDlgCtrlID(control) == IDC_PASSWORD_ERROR) {
            const auto dc = reinterpret_cast<HDC>(wparam);
            // Red error text on the dialog background, left of OK.
            ::SetTextColor(dc, RGB(196, 43, 28));
            ::SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(::GetSysColorBrush(COLOR_3DFACE));
        }
        break;
    }
    case WM_COMMAND: {
        const auto command = LOWORD(wparam);
        if (command == IDOK) {
            auto* state =
                reinterpret_cast<PasswordDialogState*>(::GetWindowLongPtrW(dialog, DWLP_USER));
            const HWND edit = ::GetDlgItem(dialog, IDC_PASSWORD_EDIT);
            if (state == nullptr || state->password == nullptr || state->validator == nullptr ||
                edit == nullptr) {
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            const int length = ::GetWindowTextLengthW(edit);
            std::wstring wide;
            if (length > 0) {
                wide.resize(static_cast<std::size_t>(length) + 1U);
                const int copied =
                    ::GetWindowTextW(edit, wide.data(), static_cast<int>(wide.size()));
                if (copied < 0) {
                    wipe_wide(wide);
                    ::EndDialog(dialog, IDCANCEL);
                    return TRUE;
                }
                wide.resize(static_cast<std::size_t>(copied));
            }
            if (!state->password->assign_from_wide(wide)) {
                wipe_wide(wide);
                ::EndDialog(dialog, IDCANCEL);
                return TRUE;
            }
            wipe_wide(wide);
            HRESULT validation_hr = E_FAIL;
            try {
                validation_hr = (*state->validator)(state->password->view());
            } catch (...) {
                validation_hr = E_UNEXPECTED;
            }
            if (SUCCEEDED(validation_hr)) {
                ::SetWindowTextW(edit, L"");
                ::EndDialog(dialog, IDOK);
                return TRUE;
            }
            if (validation_hr == HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED)) {
                // Stay on the dialog; show red error text to the left of OK (no MessageBox).
                state->password->clear();
                ::SetWindowTextW(edit, L"");
                show_password_error(dialog);
                ::SetFocus(edit);
                return TRUE;
            }
            state->password->clear();
            ::SetWindowTextW(edit, L"");
            ::EndDialog(dialog, kValidationFailed);
            return TRUE;
        }
        if (command == IDCANCEL) {
            const HWND edit = ::GetDlgItem(dialog, IDC_PASSWORD_EDIT);
            if (edit != nullptr) {
                ::SetWindowTextW(edit, L"");
            }
            ::EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    default:
        break;
    }
    return FALSE;
}

[[nodiscard]] HWND resolve_dialog_owner() noexcept {
    HWND owner = ::GetForegroundWindow();
    if (owner == nullptr) {
        owner = ::GetActiveWindow();
    }
    return owner;
}

} // namespace

PasswordDialogResult prompt_archive_password(const std::wstring_view archive_name,
                                             SecurePassword& password,
                                             const PasswordValidator& validator) {
    password.clear();
    PasswordDialogState state{archive_name, &password, &validator};
    const HINSTANCE instance = ATL::_AtlBaseModule.GetResourceInstance();
    const INT_PTR result =
        ::DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_PASSWORD), resolve_dialog_owner(),
                          password_dialog_proc, reinterpret_cast<LPARAM>(&state));
    if (result == IDOK) {
        return PasswordDialogResult::kOk;
    }
    password.clear();
    if (result == kValidationFailed) {
        return PasswordDialogResult::kFailed;
    }
    return PasswordDialogResult::kCancelled;
}

} // namespace aegra::shell
