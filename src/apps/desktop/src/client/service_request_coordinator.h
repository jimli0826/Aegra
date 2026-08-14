#pragma once

#include <QByteArray>
#include <QHash>
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

// Tracks concurrent in-flight Service requests by correlation ID. Supports simultaneous
// Repository and Job pagination without serializing unrelated domains onto one slot.
class ServiceRequestCoordinator final : public QObject {
    Q_OBJECT

  public:
    using ResponseHandler = std::function<RequestDisposition(const QByteArray& frame_body)>;

    static constexpr int kDefaultDeadlineMilliseconds = 30'000;

    explicit ServiceRequestCoordinator(IpcFrameTransport& transport, QObject* parent = nullptr);

    [[nodiscard]] bool has_pending_request() const noexcept;
    [[nodiscard]] bool has_pending_request(const QString& request_id) const noexcept;
    [[nodiscard]] int pending_count() const noexcept;

    [[nodiscard]] bool begin_request(const QString& request_id, const QByteArray& body,
                                     ResponseHandler handler,
                                     int deadline_ms = kDefaultDeadlineMilliseconds);
    // Replaces the correlation ID of an existing in-flight request (pagination continue).
    [[nodiscard]] bool continue_request(const QString& previous_request_id,
                                        const QString& request_id, const QByteArray& body,
                                        int deadline_ms = kDefaultDeadlineMilliseconds);
    void finish_request(const QString& request_id);
    void cancel_all(const QString& reason_code);

  signals:
    void request_failed(const QString& message_code);

  private:
    struct PendingRequest final {
        ResponseHandler handler;
        QTimer* deadline_timer{nullptr};
        int deadline_ms{kDefaultDeadlineMilliseconds};
        int request_kind{0};
    };

    void on_frame_received(const QByteArray& body);
    void on_transport_disconnected();
    void arm_deadline(const QString& request_id, PendingRequest& pending);
    void clear_request(const QString& request_id);
    void clear_all();

    IpcFrameTransport& transport_;
    QHash<QString, PendingRequest*> pending_;
};

// Posts work onto the Qt thread that owns receiver so task-event delivery cannot update models
// after the owner is destroyed.
void post_to_object(QObject* receiver, std::function<void()> work);

[[nodiscard]] QString extract_response_request_id(const QByteArray& body);

} // namespace aegra::desktop
