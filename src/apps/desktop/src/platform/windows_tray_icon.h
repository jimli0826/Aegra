#pragma once

#include <QObject>
#include <QString>

#include <memory>

namespace aegra::desktop {

/// Notification-area icon for the Desktop process. Windows Shell_NotifyIcon only;
/// the header stays free of Win32 types.
class WindowsTrayIcon final : public QObject {
    Q_OBJECT

  public:
    explicit WindowsTrayIcon(QObject* parent = nullptr);
    ~WindowsTrayIcon() override;

    WindowsTrayIcon(const WindowsTrayIcon&) = delete;
    WindowsTrayIcon& operator=(const WindowsTrayIcon&) = delete;

    [[nodiscard]] bool start();
    void stop();
    void retranslate();
    [[nodiscard]] bool active() const noexcept;

  signals:
    void showRequested();
    void quitRequested();

  private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::desktop
