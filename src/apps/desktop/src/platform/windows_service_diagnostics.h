#pragma once

#include <QObject>
#include <QString>

namespace aegra::desktop {

class WindowsServiceDiagnostics final : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString messageId READ messageId NOTIFY changed)

  public:
    explicit WindowsServiceDiagnostics(QObject* parent = nullptr);

    [[nodiscard]] QString messageId() const;
    Q_INVOKABLE void diagnose();
    Q_INVOKABLE void reset();

  signals:
    void changed();

  private:
    void set_message_id(QString message_id);

    QString message_id_;
};

} // namespace aegra::desktop
