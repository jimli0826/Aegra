#include "platform/windows_tray_icon.h"

#if defined(Q_OS_WIN)
#include <Windows.h>
#include <shellapi.h>
#ifndef NIF_SHOWTIP
#define NIF_SHOWTIP 0x00000080
#endif
#ifndef NOTIFYICON_VERSION_4
#define NOTIFYICON_VERSION_4 4
#endif
#ifndef NIN_SELECT
#define NIN_SELECT (WM_USER + 0)
#endif
#ifndef NIN_KEYSELECT
#define NIN_KEYSELECT (WM_USER + 1)
#endif
#endif

#include <QCoreApplication>
#include <QMetaObject>
#include <QString>

#include <cstddef>

namespace aegra::desktop {
namespace {

#if defined(Q_OS_WIN)
constexpr wchar_t kWindowClass[] = L"AegraDesktopTray";
constexpr UINT kCallbackMessage = WM_APP + 32;
constexpr UINT kOpenCommand = 1;
constexpr UINT kQuitCommand = 2;
constexpr UINT kIconId = 1;

[[nodiscard]] QString open_label() {
    //% "Open Aegra"
    return qtTrId("aegra.shell.tray.open");
}

[[nodiscard]] QString quit_label() {
    //% "Quit"
    return qtTrId("aegra.common.quit");
}

[[nodiscard]] QString tooltip_text() {
    //% "Aegra"
    return qtTrId("aegra.app.title");
}

void write_tip(wchar_t* dest, const std::size_t dest_chars, const QString& text) {
    if (dest == nullptr || dest_chars == 0) {
        return;
    }
    const QString clipped = text.left(static_cast<int>(dest_chars - 1));
    const auto* src = reinterpret_cast<const wchar_t*>(clipped.utf16());
    const auto length = static_cast<std::size_t>(clipped.size());
    for (std::size_t i = 0; i < length; ++i) {
        dest[i] = src[i];
    }
    dest[length] = L'\0';
}

[[nodiscard]] HICON load_small_icon() {
    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) {
        return nullptr;
    }
    HICON small_icon = nullptr;
    if (ExtractIconExW(path, 0, nullptr, &small_icon, 1) > 0 && small_icon != nullptr) {
        return small_icon;
    }
    HICON large_icon = nullptr;
    if (ExtractIconExW(path, 0, &large_icon, nullptr, 1) > 0) {
        return large_icon;
    }
    return nullptr;
}
#endif

} // namespace

#if defined(Q_OS_WIN)
class WindowsTrayIcon::Impl final {
  public:
    explicit Impl(WindowsTrayIcon* owner) noexcept : owner_(owner) {}
    ~Impl() { stop(); }

    Impl(const Impl&) = delete;
    Impl& operator=(const Impl&) = delete;

    [[nodiscard]] bool start();
    void stop();
    void retranslate();
    [[nodiscard]] bool active() const noexcept { return added_; }

  private:
    static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
    [[nodiscard]] static bool register_class();
    [[nodiscard]] bool create_message_window();
    [[nodiscard]] bool add_notify_icon();
    void handle_notify(LPARAM lparam);
    void handle_command(WPARAM wparam);
    void show_menu();
    void fill_icon_data(NOTIFYICONDATAW& data) const;

    WindowsTrayIcon* owner_{nullptr};
    HWND hwnd_{nullptr};
    HICON icon_{nullptr};
    bool added_{false};
};

LRESULT CALLBACK WindowsTrayIcon::Impl::wnd_proc(HWND hwnd, const UINT message,
                                                 const WPARAM wparam, const LPARAM lparam) {
    auto* impl = reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (message == kCallbackMessage && impl != nullptr) {
        impl->handle_notify(lparam);
        return 0;
    }
    if (message == WM_COMMAND && impl != nullptr) {
        impl->handle_command(wparam);
        return 0;
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

bool WindowsTrayIcon::Impl::register_class() {
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    if (GetClassInfoExW(GetModuleHandleW(nullptr), kWindowClass, &wc) != FALSE) {
        return true;
    }
    wc = {};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kWindowClass;
    return RegisterClassExW(&wc) != 0 || GetLastError() == ERROR_CLASS_ALREADY_EXISTS;
}

bool WindowsTrayIcon::Impl::create_message_window() {
    hwnd_ = CreateWindowExW(0, kWindowClass, L"AegraTray", 0, 0, 0, 0, 0, HWND_MESSAGE,
                            nullptr, GetModuleHandleW(nullptr), nullptr);
    if (hwnd_ == nullptr) {
        hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW, kWindowClass, L"AegraTray", WS_POPUP, 0, 0,
                                0, 0, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    }
    if (hwnd_ == nullptr) {
        return false;
    }
    SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
    return true;
}

void WindowsTrayIcon::Impl::fill_icon_data(NOTIFYICONDATAW& data) const {
    data.cbSize = sizeof(data);
    data.hWnd = hwnd_;
    data.uID = kIconId;
    data.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    data.uCallbackMessage = kCallbackMessage;
    data.hIcon = icon_;
    data.uVersion = NOTIFYICON_VERSION_4;
    write_tip(data.szTip, sizeof(data.szTip) / sizeof(data.szTip[0]), tooltip_text());
}

bool WindowsTrayIcon::Impl::add_notify_icon() {
    NOTIFYICONDATAW data{};
    fill_icon_data(data);
    if (Shell_NotifyIconW(NIM_ADD, &data) == FALSE) {
        return false;
    }
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
    added_ = true;
    return true;
}

bool WindowsTrayIcon::Impl::start() {
    if (added_) {
        return true;
    }
    if (!register_class() || !create_message_window()) {
        stop();
        return false;
    }
    icon_ = load_small_icon();
    if (icon_ == nullptr || !add_notify_icon()) {
        stop();
        return false;
    }
    return true;
}

void WindowsTrayIcon::Impl::stop() {
    if (added_ && hwnd_ != nullptr) {
        NOTIFYICONDATAW data{};
        data.cbSize = sizeof(data);
        data.hWnd = hwnd_;
        data.uID = kIconId;
        Shell_NotifyIconW(NIM_DELETE, &data);
        added_ = false;
    }
    if (hwnd_ != nullptr) {
        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
        DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (icon_ != nullptr) {
        DestroyIcon(icon_);
        icon_ = nullptr;
    }
}

void WindowsTrayIcon::Impl::retranslate() {
    if (!added_ || hwnd_ == nullptr) {
        return;
    }
    NOTIFYICONDATAW data{};
    fill_icon_data(data);
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void WindowsTrayIcon::Impl::handle_notify(const LPARAM lparam) {
    const UINT notify = LOWORD(lparam);
    if (notify == NIN_SELECT || notify == NIN_KEYSELECT || notify == WM_LBUTTONDBLCLK ||
        notify == WM_LBUTTONUP) {
        QMetaObject::invokeMethod(owner_, &WindowsTrayIcon::showRequested, Qt::QueuedConnection);
        return;
    }
    if (notify == WM_CONTEXTMENU || notify == WM_RBUTTONUP) {
        show_menu();
    }
}

void WindowsTrayIcon::Impl::handle_command(const WPARAM wparam) {
    const UINT command = LOWORD(wparam);
    if (command == kOpenCommand) {
        QMetaObject::invokeMethod(owner_, &WindowsTrayIcon::showRequested, Qt::QueuedConnection);
    } else if (command == kQuitCommand) {
        QMetaObject::invokeMethod(owner_, &WindowsTrayIcon::quitRequested, Qt::QueuedConnection);
    }
}

void WindowsTrayIcon::Impl::show_menu() {
    POINT cursor{};
    GetCursorPos(&cursor);
    HMENU menu = CreatePopupMenu();
    if (menu == nullptr) {
        return;
    }
    AppendMenuW(menu, MF_STRING, kOpenCommand,
                reinterpret_cast<LPCWSTR>(open_label().utf16()));
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kQuitCommand,
                reinterpret_cast<LPCWSTR>(quit_label().utf16()));
    SetForegroundWindow(hwnd_);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, cursor.x,
                   cursor.y, 0, hwnd_, nullptr);
    PostMessageW(hwnd_, WM_NULL, 0, 0);
    DestroyMenu(menu);
}
#else
class WindowsTrayIcon::Impl final {};
#endif

WindowsTrayIcon::WindowsTrayIcon(QObject* parent) : QObject(parent) {
#if defined(Q_OS_WIN)
    impl_ = std::make_unique<Impl>(this);
#endif
}

WindowsTrayIcon::~WindowsTrayIcon() = default;

bool WindowsTrayIcon::start() {
#if defined(Q_OS_WIN)
    return impl_ != nullptr && impl_->start();
#else
    return false;
#endif
}

void WindowsTrayIcon::stop() {
#if defined(Q_OS_WIN)
    if (impl_ != nullptr) {
        impl_->stop();
    }
#endif
}

void WindowsTrayIcon::retranslate() {
#if defined(Q_OS_WIN)
    if (impl_ != nullptr) {
        impl_->retranslate();
    }
#endif
}

bool WindowsTrayIcon::active() const noexcept {
#if defined(Q_OS_WIN)
    return impl_ != nullptr && impl_->active();
#else
    return false;
#endif
}

} // namespace aegra::desktop
