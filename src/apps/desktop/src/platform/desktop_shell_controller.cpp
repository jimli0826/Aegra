#include "platform/desktop_shell_controller.h"

#include <QCloseEvent>
#include <QCoreApplication>
#include <QEvent>
#include <QSettings>
#include <QWindow>
#include <QtGlobal>

namespace aegra::desktop {
namespace {

constexpr auto kCloseActionKey = "ui/closeAction";
constexpr auto kCloseHide = "hide";
constexpr auto kCloseQuit = "quit";

} // namespace

DesktopShellController::DesktopShellController(QObject* parent) : QObject(parent) {
    load_close_action();
    QObject::connect(&tray_, &WindowsTrayIcon::showRequested, this,
                     &DesktopShellController::showMainWindow);
    QObject::connect(&tray_, &WindowsTrayIcon::quitRequested, this,
                     &DesktopShellController::quit);
}

QString DesktopShellController::closeAction() const { return close_action_; }

bool DesktopShellController::is_supported_close_action(const QString& action) {
    return action == QLatin1String(kCloseHide) || action == QLatin1String(kCloseQuit);
}

bool DesktopShellController::close_hides_to_tray() const noexcept {
    return close_action_ == QLatin1String(kCloseHide);
}

void DesktopShellController::load_close_action() {
    QSettings settings;
    const auto action =
        settings.value(QLatin1String(kCloseActionKey), QStringLiteral("hide")).toString();
    close_action_ = is_supported_close_action(action) ? action : QStringLiteral("hide");
}

void DesktopShellController::save_close_action() const {
    QSettings settings;
    settings.setValue(QLatin1String(kCloseActionKey), close_action_);
}

void DesktopShellController::setCloseAction(const QString& action) {
    if (!is_supported_close_action(action) || close_action_ == action) {
        return;
    }
    close_action_ = action;
    save_close_action();
    emit closeActionChanged();
}

void DesktopShellController::attach(QWindow* window) {
    if (window_ != nullptr) {
        window_->removeEventFilter(this);
    }
    window_ = window;
    if (window_ == nullptr) {
        return;
    }
    window_->installEventFilter(this);
    if (!tray_.start()) {
        qWarning("Aegra desktop: notification tray icon was not created");
    }
}

void DesktopShellController::retranslate() { tray_.retranslate(); }

void DesktopShellController::showMainWindow() {
    if (window_ == nullptr) {
        return;
    }
    if (window_->windowState() & Qt::WindowMinimized) {
        window_->showNormal();
    } else {
        window_->show();
    }
    window_->raise();
    window_->requestActivate();
}

void DesktopShellController::hideToTray() {
    if (window_ == nullptr) {
        return;
    }
    if (!tray_.active() && !tray_.start()) {
        quit();
        return;
    }
    window_->hide();
}

void DesktopShellController::quit() {
    if (quitting_) {
        return;
    }
    quitting_ = true;
    tray_.stop();
    QCoreApplication::quit();
}

bool DesktopShellController::eventFilter(QObject* watched, QEvent* event) {
    if (watched != window_ || event == nullptr || event->type() != QEvent::Close) {
        return QObject::eventFilter(watched, event);
    }
    if (quitting_) {
        return false;
    }
    auto* close = static_cast<QCloseEvent*>(event);
    if (!close_hides_to_tray()) {
        quit();
        return false;
    }
    hideToTray();
    close->ignore();
    return true;
}

} // namespace aegra::desktop
