#pragma once

#include "platform/windows_tray_icon.h"

#include <QObject>
#include <QPointer>
#include <QString>

class QWindow;

namespace aegra::desktop {

/// Close-button policy for the Desktop main window. `closeAction` is `hide`
/// (notification area) or `quit`, persisted in Desktop QSettings `ui/closeAction`.
class DesktopShellController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString closeAction READ closeAction WRITE setCloseAction NOTIFY
                   closeActionChanged)

  public:
    explicit DesktopShellController(QObject* parent = nullptr);

    void attach(QWindow* window);
    void retranslate();

    [[nodiscard]] QString closeAction() const;
    Q_INVOKABLE void setCloseAction(const QString& action);

    Q_INVOKABLE void showMainWindow();
    Q_INVOKABLE void hideToTray();
    Q_INVOKABLE void quit();

  signals:
    void closeActionChanged();

  protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

  private:
    void load_close_action();
    void save_close_action() const;
    [[nodiscard]] static bool is_supported_close_action(const QString& action);
    [[nodiscard]] bool close_hides_to_tray() const noexcept;

    QPointer<QWindow> window_;
    WindowsTrayIcon tray_;
    QString close_action_{QStringLiteral("hide")};
    bool quitting_{false};
};

} // namespace aegra::desktop
