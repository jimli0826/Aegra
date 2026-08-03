#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

#include <cstdint>
#include <functional>

class QTimer;

namespace aegra::desktop {

class IpcFrameTransport;

enum class RequestDisposition : std::uint8_t {
    kFinished,
    kContinue,
    kProtocolError,
};

// Tracks in-flight service requests with a unique correlation ID, deadline, and disconnect
// cleanup. Pagination uses continue_request() to replace the ID while keeping the same handler.
class ServiceRequestCoordinator final : public QObject {
    Q_OBJECT

  public:
    using ResponseHandler = std::function<RequestDisposition(const QByteArray& frame_body)>;

    static constexpr int kDefaultDeadlineMilliseconds = 30'000;

    explicit ServiceRequestCoordinator(IpcFrameTransport& transport, QObject* parent = nullptr);

    [[nodiscard]] bool has_pending_request() const noexcept;
    [[nodiscard]] QString pending_request_id() const;

    [[nodiscard]] bool begin_request(const QString& request_id, const QByteArray& body,
                                     ResponseHandler handler,
                                     int deadline_ms = kDefaultDeadlineMilliseconds);
    [[nodiscard]] bool continue_request(const QString& request_id, const QByteArray& body,
                                        int deadline_ms = kDefaultDeadlineMilliseconds);
    void finish_request();
    void cancel_pending(const QString& reason_code);

  signals:
    void request_failed(const QString& message_code);

  private:
    void on_frame_received(const QByteArray& body);
    void on_deadline();
    void on_transport_disconnected();
    void clear_pending();

    IpcFrameTransport& transport_;
    QTimer* deadline_timer_{nullptr};
    QString request_id_;
    ResponseHandler handler_;
    bool pending_{false};
    int deadline_ms_{kDefaultDeadlineMilliseconds};
};

// Posts work onto the Qt thread that owns receiver so task-event delivery cannot update models
// after the owner is destroyed.
void post_to_object(QObject* receiver, std::function<void()> work);

} // namespace aegra::desktop
