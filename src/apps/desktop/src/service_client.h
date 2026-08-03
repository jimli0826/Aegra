#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QStringList>

#include <cstdint>

class QJsonObject;
class QLocalSocket;
class QTimer;

class ServiceClient final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool connected READ connected NOTIFY stateChanged)
    Q_PROPERTY(QString statusText READ statusText NOTIFY stateChanged)
    Q_PROPERTY(QString serviceVersion READ serviceVersion NOTIFY stateChanged)
    Q_PROPERTY(quint32 apiVersion READ apiVersion NOTIFY stateChanged)
    Q_PROPERTY(QStringList capabilities READ capabilities NOTIFY stateChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY stateChanged)

  public:
    explicit ServiceClient(QObject* parent = nullptr);

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] QString serviceVersion() const;
    [[nodiscard]] quint32 apiVersion() const noexcept;
    [[nodiscard]] QStringList capabilities() const;
    [[nodiscard]] QString errorText() const;

    Q_INVOKABLE void reconnect();

  signals:
    void stateChanged();

  private:
    enum class State : std::uint8_t {
        kDisconnected,
        kConnecting,
        kReady,
    };

    void on_connected();
    void on_disconnected();
    void on_ready_read();
    void on_socket_error();
    void send_service_info_request();
    void consume_frames();
    [[nodiscard]] bool apply_response(const QByteArray& frame);
    [[nodiscard]] bool apply_service_info(const QJsonObject& root);
    void fail_protocol();
    void schedule_reconnect();
    void set_state(State state, QString error = {});

    QLocalSocket* socket_{nullptr};
    QTimer* reconnect_timer_{nullptr};
    QByteArray input_;
    quint32 expected_frame_bytes_{0};
    QString request_id_;
    QString service_version_;
    QStringList capabilities_;
    QString error_text_;
    quint32 api_version_{0};
    State state_{State::kDisconnected};
};
