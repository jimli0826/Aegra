#include "pe_ui.h"

#include "pe_run.h"
#include "pe_strings.h"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <cwchar>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aegra::apps::pe_restore {
namespace {

inline constexpr UINT kMessageStage = WM_APP + 1;
inline constexpr UINT kMessageProgress = WM_APP + 2;
inline constexpr UINT kMessageShowSummary = WM_APP + 3;
inline constexpr UINT kMessageShowPassword = WM_APP + 4;
inline constexpr UINT kMessageDone = WM_APP + 5;

inline constexpr UINT_PTR kTimerCountdown = 1;
inline constexpr UINT_PTR kTimerReboot = 2;

inline constexpr int kButtonStart = 100;
inline constexpr int kButtonCancel = 101;
inline constexpr int kButtonPasswordOk = 102;
inline constexpr int kEditPassword = 103;

inline constexpr COLORREF kColorBackground = RGB(18, 20, 26);
inline constexpr COLORREF kColorText = RGB(230, 232, 238);
inline constexpr COLORREF kColorMuted = RGB(150, 156, 168);
inline constexpr COLORREF kColorAccent = RGB(64, 140, 255);
inline constexpr COLORREF kColorWarning = RGB(255, 170, 60);
inline constexpr COLORREF kColorError = RGB(255, 90, 90);
inline constexpr COLORREF kColorBarTrack = RGB(45, 50, 60);

[[nodiscard]] std::wstring to_wide(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto required = MultiByteToWideChar(CP_UTF8, 0, value.data(),
                                              static_cast<int>(value.size()), nullptr, 0);
    if (required <= 0) {
        return L"?";
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), wide.data(),
                        required);
    return wide;
}

enum class UiPage : std::uint8_t {
    kBusy = 1,
    kSummary = 2,
    kPassword = 3,
    kRunning = 4,
    kDone = 5,
};

/// UI-side state; every field is owned by the UI thread except the bridge
/// signal data, which is guarded by `mutex`.
class RecoveryWindow final : public IPeRunView {
  public:
    [[nodiscard]] int run();

    // IPeRunView (called on the flow thread).
    void stage_changed(PeRunStage stage) override {
        PostMessage(window_, kMessageStage, static_cast<WPARAM>(stage), 0);
    }

    [[nodiscard]] PeUserDecision wait_start_decision(const PeRunSummary& summary) override {
        {
            const std::lock_guard lock(mutex_);
            summary_ = summary;
        }
        PostMessage(window_, kMessageShowSummary, 0, 0);
        WaitForSingleObject(decision_event_, INFINITE);
        const std::lock_guard lock(mutex_);
        return decision_;
    }

    [[nodiscard]] base::Result<std::string> wait_password() override {
        PostMessage(window_, kMessageShowPassword, 0, 0);
        WaitForSingleObject(password_event_, INFINITE);
        const std::lock_guard lock(mutex_);
        if (password_cancelled_) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kCancelled, "password entry cancelled"});
        }
        auto password = std::move(password_);
        password_.clear();
        return base::Result<std::string>::success(std::move(password));
    }

    void progress_changed(const PeRunProgress& progress) override {
        const WPARAM packed = progress.has_percent ? progress.percent : 0xFFFFFFFFU;
        PostMessage(window_, kMessageProgress, packed,
                    static_cast<LPARAM>(progress.written_bytes / (1024ULL * 1024ULL)));
    }

  private:
    static LRESULT CALLBACK window_procedure(HWND window, UINT message, WPARAM wparam,
                                             LPARAM lparam);
    [[nodiscard]] LRESULT handle_message(UINT message, WPARAM wparam, LPARAM lparam);
    void create_controls();
    void layout_controls() const;
    void show_page(UiPage page);
    void paint() const;
    void paint_content(HDC context, const RECT& client) const;
    void handle_command(int control);
    void handle_timer(UINT_PTR timer);
    void submit_password(bool cancelled);
    void finish_decision(PeUserDecision decision);
    void begin_reboot_countdown();
    [[nodiscard]] const wchar_t* text(StringId id) const { return pe_string(id, locale_); }

    HWND window_{nullptr};
    HWND button_start_{nullptr};
    HWND button_cancel_{nullptr};
    HWND button_password_ok_{nullptr};
    HWND edit_password_{nullptr};
    HFONT font_title_{nullptr};
    HFONT font_text_{nullptr};
    HANDLE decision_event_{nullptr};
    HANDLE password_event_{nullptr};

    UiPage page_{UiPage::kBusy};
    PeRunStage stage_{PeRunStage::kLocatingJob};
    std::uint32_t countdown_remaining_{0};
    std::uint32_t reboot_remaining_{0};
    bool progress_has_percent_{false};
    std::uint32_t progress_percent_{0};
    std::uint64_t progress_written_mb_{0};
    std::string locale_{"en-US"};
    PeRunOutcome outcome_;
    bool flow_finished_{false};

    mutable std::mutex mutex_;
    PeRunSummary summary_;
    PeUserDecision decision_{PeUserDecision::kCancel};
    std::string password_;
    bool password_cancelled_{false};
};

RecoveryWindow* g_window_instance = nullptr;

LRESULT CALLBACK RecoveryWindow::window_procedure(const HWND window, const UINT message,
                                                  const WPARAM wparam, const LPARAM lparam) {
    if (g_window_instance != nullptr && g_window_instance->window_ == window) {
        return g_window_instance->handle_message(message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

int RecoveryWindow::run() {
    g_window_instance = this;
    decision_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    password_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    WNDCLASSW window_class{};
    window_class.lpfnWndProc = window_procedure;
    window_class.hInstance = GetModuleHandleW(nullptr);
    // MAKEINTRESOURCEW(32512) == IDC_ARROW; the ANSI macro form does not compile here.
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    window_class.hbrBackground = CreateSolidBrush(kColorBackground);
    window_class.lpszClassName = L"AegraPeRecoveryWindow";
    RegisterClassW(&window_class);
    const int width = GetSystemMetrics(SM_CXSCREEN);
    const int height = GetSystemMetrics(SM_CYSCREEN);
    const HWND window =
        CreateWindowExW(WS_EX_TOPMOST, window_class.lpszClassName, L"Aegra Recovery", WS_POPUP,
                        0, 0, width, height, nullptr, nullptr, window_class.hInstance, nullptr);
    if (window == nullptr) {
        return 1;
    }
    window_ = window;
    font_title_ = CreateFontW(-34, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0,
                              0, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    font_text_ = CreateFontW(-20, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, 0, 0,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    create_controls();
    show_page(UiPage::kBusy);
    ShowWindow(window_, SW_SHOW);

    std::thread flow_thread([this]() {
        auto outcome = run_pe_restore_flow(*this);
        {
            const std::lock_guard lock(mutex_);
            outcome_ = std::move(outcome);
        }
        PostMessage(window_, kMessageDone, 0, 0);
    });

    MSG message{};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_RETURN &&
            page_ == UiPage::kPassword) {
            handle_command(kButtonPasswordOk);
            continue;
        }
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    flow_thread.join();
    CloseHandle(decision_event_);
    CloseHandle(password_event_);
    g_window_instance = nullptr;
    return outcome_.kind == PeRunResultKind::kFailed ? 1 : 0;
}

void RecoveryWindow::create_controls() {
    const auto make_button = [this](const int id) {
        return CreateWindowExW(0, L"BUTTON", L"", WS_CHILD | BS_PUSHBUTTON, 0, 0, 10, 10, window_,
                               // NOLINTNEXTLINE(performance-no-int-to-ptr) Win32 control id.
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                               GetModuleHandleW(nullptr), nullptr);
    };
    button_start_ = make_button(kButtonStart);
    button_cancel_ = make_button(kButtonCancel);
    button_password_ok_ = make_button(kButtonPasswordOk);
    edit_password_ = CreateWindowExW(
        0, L"EDIT", L"", WS_CHILD | WS_BORDER | ES_PASSWORD | ES_AUTOHSCROLL, 0, 0, 10, 10,
        // NOLINTNEXTLINE(performance-no-int-to-ptr) Win32 control id.
        window_, reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEditPassword)),
        GetModuleHandleW(nullptr), nullptr);
    for (const HWND control :
         {button_start_, button_cancel_, button_password_ok_, edit_password_}) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_text_), TRUE);
    }
}

void RecoveryWindow::layout_controls() const {
    RECT client{};
    GetClientRect(window_, &client);
    const int width = client.right - client.left;
    const int height = client.bottom - client.top;
    const int margin = width / 12;
    const int button_width = width / 5;
    const int button_height = 52;
    const int row = height - height / 5;
    MoveWindow(button_start_, margin, row, button_width, button_height, TRUE);
    MoveWindow(button_cancel_, margin + button_width + 24, row, button_width + 80, button_height,
               TRUE);
    MoveWindow(edit_password_, margin, height / 2, width / 3, 40, TRUE);
    MoveWindow(button_password_ok_, margin, height / 2 + 56, button_width, button_height, TRUE);
}

void RecoveryWindow::show_page(const UiPage page) {
    page_ = page;
    layout_controls();
    const bool summary = page == UiPage::kSummary;
    const bool password = page == UiPage::kPassword;
    ShowWindow(button_start_, summary ? SW_SHOW : SW_HIDE);
    ShowWindow(button_cancel_, summary || password ? SW_SHOW : SW_HIDE);
    ShowWindow(button_password_ok_, password ? SW_SHOW : SW_HIDE);
    ShowWindow(edit_password_, password ? SW_SHOW : SW_HIDE);
    SetWindowTextW(button_start_, text(StringId::kStartNow));
    SetWindowTextW(button_cancel_, text(StringId::kCancelAndReboot));
    SetWindowTextW(button_password_ok_, text(StringId::kPasswordConfirm));
    if (password) {
        SetFocus(edit_password_);
    }
    InvalidateRect(window_, nullptr, TRUE);
}

LRESULT RecoveryWindow::handle_message(const UINT message, const WPARAM wparam,
                                       const LPARAM lparam) {
    switch (message) {
    case WM_PAINT:
        paint();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_COMMAND:
        handle_command(LOWORD(wparam));
        return 0;
    case WM_TIMER:
        handle_timer(wparam);
        return 0;
    case kMessageStage:
        stage_ = static_cast<PeRunStage>(wparam);
        InvalidateRect(window_, nullptr, TRUE);
        return 0;
    case kMessageProgress:
        progress_has_percent_ = wparam != 0xFFFFFFFFU;
        progress_percent_ = progress_has_percent_ ? static_cast<std::uint32_t>(wparam) : 0;
        progress_written_mb_ = static_cast<std::uint64_t>(lparam);
        if (page_ != UiPage::kRunning) {
            show_page(UiPage::kRunning);
        }
        InvalidateRect(window_, nullptr, FALSE);
        return 0;
    case kMessageShowSummary: {
        const std::lock_guard lock(mutex_);
        locale_ = summary_.locale.empty() ? locale_ : summary_.locale;
        countdown_remaining_ = summary_.auto_start_seconds;
    }
        show_page(UiPage::kSummary);
        if (countdown_remaining_ == 0) {
            finish_decision(PeUserDecision::kStart);
        } else {
            SetTimer(window_, kTimerCountdown, 1000, nullptr);
        }
        return 0;
    case kMessageShowPassword:
        show_page(UiPage::kPassword);
        return 0;
    case kMessageDone: {
        {
            const std::lock_guard lock(mutex_);
            locale_ = outcome_.locale.empty() ? locale_ : outcome_.locale;
            flow_finished_ = true;
        }
        show_page(UiPage::kDone);
        if (outcome_.reboot) {
            begin_reboot_countdown();
        }
        return 0;
    }
    case WM_CLOSE:
        // The recovery window cannot be closed; the flow decides when to leave.
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(window_, message, wparam, lparam);
}

void RecoveryWindow::handle_command(const int control) {
    if (page_ == UiPage::kSummary && control == kButtonStart) {
        finish_decision(PeUserDecision::kStart);
        return;
    }
    if (page_ == UiPage::kSummary && control == kButtonCancel) {
        finish_decision(PeUserDecision::kCancel);
        return;
    }
    if (page_ == UiPage::kPassword && control == kButtonPasswordOk) {
        submit_password(false);
        return;
    }
    if (page_ == UiPage::kPassword && control == kButtonCancel) {
        submit_password(true);
    }
}

void RecoveryWindow::finish_decision(const PeUserDecision decision) {
    KillTimer(window_, kTimerCountdown);
    {
        const std::lock_guard lock(mutex_);
        decision_ = decision;
    }
    show_page(UiPage::kBusy);
    SetEvent(decision_event_);
}

void RecoveryWindow::submit_password(const bool cancelled) {
    std::wstring buffer;
    buffer.resize(512);
    const int length = GetWindowTextW(edit_password_, buffer.data(),
                                      static_cast<int>(buffer.size()));
    buffer.resize(static_cast<std::size_t>((std::max)(length, 0)));
    SetWindowTextW(edit_password_, L"");
    std::string password;
    if (!cancelled && !buffer.empty()) {
        const int required =
            WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.size()),
                                nullptr, 0, nullptr, nullptr);
        if (required > 0) {
            password.resize(static_cast<std::size_t>(required));
            WideCharToMultiByte(CP_UTF8, 0, buffer.data(), static_cast<int>(buffer.size()),
                                password.data(), required, nullptr, nullptr);
        }
    }
    SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
    {
        const std::lock_guard lock(mutex_);
        password_cancelled_ = cancelled;
        password_ = std::move(password);
    }
    show_page(UiPage::kBusy);
    SetEvent(password_event_);
}

void RecoveryWindow::handle_timer(const UINT_PTR timer) {
    if (timer == kTimerCountdown) {
        if (countdown_remaining_ > 0) {
            --countdown_remaining_;
        }
        if (countdown_remaining_ == 0) {
            finish_decision(PeUserDecision::kStart);
        } else {
            InvalidateRect(window_, nullptr, FALSE);
        }
        return;
    }
    if (timer == kTimerReboot) {
        if (reboot_remaining_ > 0) {
            --reboot_remaining_;
        }
        if (reboot_remaining_ == 0) {
            KillTimer(window_, kTimerReboot);
            reboot_winpe();
            DestroyWindow(window_);
        } else {
            InvalidateRect(window_, nullptr, FALSE);
        }
    }
}

void RecoveryWindow::begin_reboot_countdown() {
    reboot_remaining_ = 5;
    SetTimer(window_, kTimerReboot, 1000, nullptr);
}

void RecoveryWindow::paint() const {
    PAINTSTRUCT paint_state{};
    const HDC context = BeginPaint(window_, &paint_state);
    RECT client{};
    GetClientRect(window_, &client);
    const HBRUSH background = CreateSolidBrush(kColorBackground);
    FillRect(context, &client, background);
    DeleteObject(background);
    SetBkMode(context, TRANSPARENT);
    paint_content(context, client);
    EndPaint(window_, &paint_state);
}

void RecoveryWindow::paint_content(const HDC context, const RECT& client) const {
    const int width = client.right - client.left;
    const int margin = width / 12;
    int y = (client.bottom - client.top) / 6;
    const auto draw_line = [&](const wchar_t* line, const HFONT font, const COLORREF color,
                               const int advance) {
        SelectObject(context, font);
        SetTextColor(context, color);
        TextOutW(context, margin, y, line, static_cast<int>(std::wcslen(line)));
        y += advance;
    };
    draw_line(text(StringId::kWindowTitle), font_title_, kColorText, 70);
    wchar_t formatted[512]{};
    switch (page_) {
    case UiPage::kBusy: {
        const auto stage_id = stage_ == PeRunStage::kLocatingJob ? StringId::kStageLocatingJob
                              : stage_ == PeRunStage::kVerifying ? StringId::kStageVerifying
                                                                 : StringId::kStagePreparing;
        draw_line(text(stage_id), font_text_, kColorMuted, 36);
        break;
    }
    case UiPage::kSummary:
    case UiPage::kPassword: {
        draw_line(text(StringId::kSummaryHeading), font_text_, kColorText, 40);
        std::wstring backup_line;
        std::wstring target_line;
        {
            const std::lock_guard lock(mutex_);
            backup_line = std::wstring(text(StringId::kSummaryBackup)) + L"  " +
                          to_wide(summary_.backup_display);
            target_line = std::wstring(text(StringId::kSummaryTargetDisk)) + L"  " +
                          to_wide(summary_.target_display);
        }
        draw_line(backup_line.c_str(), font_text_, kColorMuted, 34);
        draw_line(target_line.c_str(), font_text_, kColorMuted, 44);
        draw_line(text(StringId::kSummaryWarning), font_text_, kColorWarning, 44);
        if (page_ == UiPage::kSummary && countdown_remaining_ > 0) {
            swprintf_s(formatted, text(StringId::kCountdownFormat), countdown_remaining_);
            draw_line(formatted, font_text_, kColorAccent, 36);
        }
        if (page_ == UiPage::kPassword) {
            draw_line(text(StringId::kPasswordPrompt), font_text_, kColorText, 36);
        }
        break;
    }
    case UiPage::kRunning: {
        draw_line(text(StringId::kStageRestoring), font_text_, kColorText, 44);
        const RECT track{margin, y, client.right - margin, y + 26};
        const HBRUSH track_brush = CreateSolidBrush(kColorBarTrack);
        FillRect(context, &track, track_brush);
        DeleteObject(track_brush);
        if (progress_has_percent_) {
            RECT fill = track;
            fill.right = fill.left + static_cast<int>((static_cast<std::int64_t>(width - 2 * margin) *
                                                       progress_percent_) /
                                                      100);
            const HBRUSH fill_brush = CreateSolidBrush(kColorAccent);
            FillRect(context, &fill, fill_brush);
            DeleteObject(fill_brush);
        }
        y += 44;
        swprintf_s(formatted, text(StringId::kProgressFormat), progress_percent_,
                   static_cast<unsigned long long>(progress_written_mb_));
        draw_line(formatted, font_text_, kColorMuted, 36);
        draw_line(text(StringId::kCancelDisabledNote), font_text_, kColorMuted, 36);
        break;
    }
    case UiPage::kDone: {
        PeRunOutcome outcome;
        {
            const std::lock_guard lock(mutex_);
            outcome = outcome_;
        }
        const auto result_id = outcome.kind == PeRunResultKind::kSuccess ? StringId::kResultSuccess
                               : outcome.kind == PeRunResultKind::kCancelled
                                   ? StringId::kResultCancelled
                                   : StringId::kResultFailed;
        const auto color = outcome.kind == PeRunResultKind::kSuccess    ? kColorText
                           : outcome.kind == PeRunResultKind::kCancelled ? kColorWarning
                                                                          : kColorError;
        draw_line(text(result_id), font_text_, color, 40);
        if (outcome.kind == PeRunResultKind::kFailed) {
            const auto code = to_wide(outcome.error_code);
            draw_line(code.c_str(), font_text_, kColorMuted, 34);
            const auto detail = to_wide(outcome.error_message);
            draw_line(detail.c_str(), font_text_, kColorMuted, 34);
        }
        if (outcome.reboot && reboot_remaining_ > 0) {
            swprintf_s(formatted, text(StringId::kRebootCountdownFormat), reboot_remaining_);
            draw_line(formatted, font_text_, kColorAccent, 36);
        }
        break;
    }
    }
}

} // namespace

int run_pe_restore_ui() {
    RecoveryWindow window;
    return window.run();
}

} // namespace aegra::apps::pe_restore
