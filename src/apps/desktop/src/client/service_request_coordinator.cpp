#include "client/service_request_coordinator.h"

#include "client/ipc_frame_transport.h"

#include <QMetaObject>
#include <QPointer>
#include <QTimer>

namespace aegra::desktop {

ServiceRequestCoordinator::ServiceRequestCoordinator(IpcFrameTransport& transport, QObject* parent)
    : QObject(parent), transport_(transport), deadline_timer_(new QTimer(this)) {
    deadline_timer_->setSingleShot(true);
    connect(deadline_timer_, &QTimer::timeout, this, &ServiceRequestCoordinator::on_deadline);
    connect(&transport_, &IpcFrameTransport::frame_received, this,
            &ServiceRequestCoordinator::on_frame_received);
    connect(&transport_, &IpcFrameTransport::disconnected, this,
            &ServiceRequestCoordinator::on_transport_disconnected);
}

bool ServiceRequestCoordinator::has_pending_request() const noexcept { return pending_; }

QString ServiceRequestCoordinator::pending_request_id() const { return request_id_; }

bool ServiceRequestCoordinator::begin_request(const QString& request_id, const QByteArray& body,
                                              ResponseHandler handler, const int deadline_ms) {
    if (pending_ || request_id.isEmpty() || !handler || !transport_.is_connected()) {
        return false;
    }
    if (!transport_.send_frame(body)) {
        return false;
    }
    request_id_ = request_id;
    handler_ = std::move(handler);
    pending_ = true;
    deadline_ms_ = deadline_ms;
    deadline_timer_->start(deadline_ms_);
    return true;
}

bool ServiceRequestCoordinator::continue_request(const QString& request_id, const QByteArray& body,
                                                 const int deadline_ms) {
    if (!pending_ || request_id.isEmpty() || !handler_ || !transport_.is_connected()) {
        return false;
    }
    if (!transport_.send_frame(body)) {
        clear_pending();
        emit request_failed(QStringLiteral("service.send_failed"));
        return false;
    }
    request_id_ = request_id;
    deadline_ms_ = deadline_ms;
    deadline_timer_->start(deadline_ms_);
    return true;
}

void ServiceRequestCoordinator::finish_request() {
    if (!pending_) {
        return;
    }
    clear_pending();
}

void ServiceRequestCoordinator::cancel_pending(const QString& reason_code) {
    if (!pending_) {
        return;
    }
    clear_pending();
    emit request_failed(reason_code);
}

void ServiceRequestCoordinator::on_transport_disconnected() {
    if (!pending_) {
        return;
    }
    clear_pending();
    emit request_failed(QStringLiteral("service.disconnected"));
}

void ServiceRequestCoordinator::on_frame_received(const QByteArray& body) {
    if (!pending_ || !handler_) {
        return;
    }
    const auto disposition = handler_(body);
    switch (disposition) {
    case RequestDisposition::kFinished:
        clear_pending();
        break;
    case RequestDisposition::kContinue:
        // Handler must call continue_request() before returning.
        break;
    case RequestDisposition::kProtocolError:
        clear_pending();
        emit request_failed(QStringLiteral("service.protocol_invalid"));
        break;
    }
}

void ServiceRequestCoordinator::on_deadline() {
    if (!pending_) {
        return;
    }
    clear_pending();
    emit request_failed(QStringLiteral("service.request_timeout"));
}

void ServiceRequestCoordinator::clear_pending() {
    deadline_timer_->stop();
    request_id_.clear();
    handler_ = nullptr;
    pending_ = false;
}

void post_to_object(QObject* receiver, std::function<void()> work) {
    if (receiver == nullptr || !work) {
        return;
    }
    QPointer guard(receiver);
    QMetaObject::invokeMethod(
        receiver,
        [guard, work = std::move(work)]() {
            if (guard.isNull()) {
                return;
            }
            work();
        },
        Qt::QueuedConnection);
}

} // namespace aegra::desktop
