#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>

class QLocalSocket;
class QTimer;

namespace aegra::desktop {

// Length-prefixed Named Pipe framing transport. Owns the socket and reconnect timer.
// Delivers complete frames only; protocol parsing lives outside this type.
class IpcFrameTransport final : public QObject {
    Q_OBJECT

  public:
    static constexpr quint32 kMaximumFrameBytes = 1024U * 1024U;
    static constexpr int kDefaultReconnectDelayMilliseconds = 2'000;
    static constexpr int kMaximumReconnectDelayMilliseconds = 30'000;

    explicit IpcFrameTransport(QString pipe_name, QObject* parent = nullptr);

    void set_reconnect_delay_milliseconds(int delay_ms);
    /// When false, connect failures do not schedule automatic reconnect (splash error until Retry).
    void set_auto_reconnect_enabled(bool enabled);
    [[nodiscard]] bool auto_reconnect_enabled() const noexcept;
    [[nodiscard]] bool is_connected() const noexcept;

    void connect_to_service();
    void disconnect_from_service();
    /// Enable auto-reconnect and schedule a connect if not already connected/connecting.
    void ensure_reconnect_scheduled();
    /// Schedule reconnect with exponential backoff (for post-Ready failure paths).
    void schedule_reconnect_with_backoff();
    [[nodiscard]] bool send_frame(const QByteArray& body);

  signals:
    void connected();
    void disconnected();
    void frame_received(const QByteArray& body);
    void transport_error(const QString& message_code);

  private:
    void on_connected();
    void on_disconnected();
    void on_ready_read();
    void on_socket_error();
    void consume_frames();
    void schedule_reconnect();
    void fail_protocol();

    QString pipe_name_;
    QLocalSocket* socket_{nullptr};
    QTimer* reconnect_timer_{nullptr};
    QByteArray input_;
    quint32 expected_frame_bytes_{0};
    bool intentional_disconnect_{false};
    bool auto_reconnect_enabled_{true};
    bool error_notified_{false};
    int reconnect_delay_ms_{kDefaultReconnectDelayMilliseconds};
    int next_reconnect_delay_ms_{kDefaultReconnectDelayMilliseconds};
};

} // namespace aegra::desktop
