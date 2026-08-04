#include "client/service_request_coordinator.h"

#include "client/ipc_frame_transport.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QMetaObject>
#include <QPointer>
#include <QTimer>

namespace aegra::desktop {

QString extract_response_request_id(const QByteArray& body) {
    const auto document = QJsonDocument::fromJson(body);
    if (!document.isObject()) {
        return {};
    }
    const auto value = document.object().value(QStringLiteral("request_id"));
    return value.isString() ? value.toString() : QString{};
}

ServiceRequestCoordinator::ServiceRequestCoordinator(IpcFrameTransport& transport, QObject* parent)
    : QObject(parent), transport_(transport) {
    connect(&transport_, &IpcFrameTransport::frame_received, this,
            &ServiceRequestCoordinator::on_frame_received);
    connect(&transport_, &IpcFrameTransport::disconnected, this,
            &ServiceRequestCoordinator::on_transport_disconnected);
}

bool ServiceRequestCoordinator::has_pending_request() const noexcept { return !pending_.isEmpty(); }

bool ServiceRequestCoordinator::has_pending_request(const QString& request_id) const noexcept {
    return pending_.contains(request_id);
}

int ServiceRequestCoordinator::pending_count() const noexcept { return pending_.size(); }

bool ServiceRequestCoordinator::begin_request(const QString& request_id, const QByteArray& body,
                                              ResponseHandler handler, const int deadline_ms) {
    if (request_id.isEmpty() || !handler || !transport_.is_connected() ||
        pending_.contains(request_id)) {
        return false;
    }
    if (!transport_.send_frame(body)) {
        return false;
    }
    auto* entry = new PendingRequest();
    entry->handler = std::move(handler);
    entry->deadline_ms = deadline_ms;
    entry->deadline_timer = new QTimer(this);
    entry->deadline_timer->setSingleShot(true);
    pending_.insert(request_id, entry);
    arm_deadline(request_id, *entry);
    return true;
}

bool ServiceRequestCoordinator::continue_request(const QString& previous_request_id,
                                                 const QString& request_id, const QByteArray& body,
                                                 const int deadline_ms) {
    if (previous_request_id.isEmpty() || request_id.isEmpty() || !transport_.is_connected() ||
        !pending_.contains(previous_request_id) || pending_.contains(request_id)) {
        return false;
    }
    if (!transport_.send_frame(body)) {
        clear_request(previous_request_id);
        emit request_failed(QStringLiteral("service.send_failed"));
        return false;
    }
    auto* entry = pending_.take(previous_request_id);
    entry->deadline_ms = deadline_ms;
    pending_.insert(request_id, entry);
    arm_deadline(request_id, *entry);
    return true;
}

void ServiceRequestCoordinator::finish_request(const QString& request_id) {
    clear_request(request_id);
}

void ServiceRequestCoordinator::cancel_all(const QString& reason_code) {
    if (pending_.isEmpty()) {
        return;
    }
    clear_all();
    emit request_failed(reason_code);
}

void ServiceRequestCoordinator::on_transport_disconnected() {
    if (pending_.isEmpty()) {
        return;
    }
    clear_all();
    emit request_failed(QStringLiteral("service.disconnected"));
}

void ServiceRequestCoordinator::on_frame_received(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    if (request_id.isEmpty() || !pending_.contains(request_id)) {
        return;
    }
    auto* entry = pending_.value(request_id);
    if (entry == nullptr || !entry->handler) {
        clear_request(request_id);
        return;
    }
    const auto disposition = entry->handler(body);
    switch (disposition) {
    case RequestDisposition::kFinished:
        clear_request(request_id);
        break;
    case RequestDisposition::kContinue:
        break;
    case RequestDisposition::kProtocolError:
        clear_request(request_id);
        emit request_failed(QStringLiteral("service.protocol_invalid"));
        break;
    }
}

void ServiceRequestCoordinator::arm_deadline(const QString& request_id, PendingRequest& pending) {
    if (pending.deadline_timer == nullptr) {
        pending.deadline_timer = new QTimer(this);
        pending.deadline_timer->setSingleShot(true);
    }
    QObject::disconnect(pending.deadline_timer, nullptr, this, nullptr);
    connect(pending.deadline_timer, &QTimer::timeout, this, [this, request_id]() {
        if (!pending_.contains(request_id)) {
            return;
        }
        clear_request(request_id);
        emit request_failed(QStringLiteral("service.request_timeout"));
    });
    pending.deadline_timer->start(pending.deadline_ms);
}

void ServiceRequestCoordinator::clear_request(const QString& request_id) {
    auto* entry = pending_.take(request_id);
    if (entry == nullptr) {
        return;
    }
    if (entry->deadline_timer != nullptr) {
        entry->deadline_timer->stop();
        entry->deadline_timer->deleteLater();
        entry->deadline_timer = nullptr;
    }
    delete entry;
}

void ServiceRequestCoordinator::clear_all() {
    const auto ids = pending_.keys();
    for (const auto& id : ids) {
        clear_request(id);
    }
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
