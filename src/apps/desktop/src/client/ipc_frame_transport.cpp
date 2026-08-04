#include "client/ipc_frame_transport.h"

#include <QLocalSocket>
#include <QTimer>

namespace aegra::desktop {
namespace {

[[nodiscard]] quint32 decode_length(const QByteArray& input) noexcept {
    return static_cast<quint32>(static_cast<unsigned char>(input[0])) |
           (static_cast<quint32>(static_cast<unsigned char>(input[1])) << 8U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[2])) << 16U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[3])) << 24U);
}

[[nodiscard]] QByteArray frame(const QByteArray& body) {
    const auto size = static_cast<quint32>(body.size());
    QByteArray result;
    result.reserve(body.size() + 4);
    result.append(static_cast<char>(size & 0xFFU));
    result.append(static_cast<char>((size >> 8U) & 0xFFU));
    result.append(static_cast<char>((size >> 16U) & 0xFFU));
    result.append(static_cast<char>((size >> 24U) & 0xFFU));
    result.append(body);
    return result;
}

} // namespace

IpcFrameTransport::IpcFrameTransport(QString pipe_name, QObject* parent)
    : QObject(parent), pipe_name_(std::move(pipe_name)), socket_(new QLocalSocket(this)),
      reconnect_timer_(new QTimer(this)) {
    reconnect_timer_->setSingleShot(true);
    reconnect_timer_->setInterval(kDefaultReconnectDelayMilliseconds);
    connect(reconnect_timer_, &QTimer::timeout, this, &IpcFrameTransport::connect_to_service);
    connect(socket_, &QLocalSocket::connected, this, &IpcFrameTransport::on_connected);
    connect(socket_, &QLocalSocket::disconnected, this, &IpcFrameTransport::on_disconnected);
    connect(socket_, &QLocalSocket::readyRead, this, &IpcFrameTransport::on_ready_read);
    connect(socket_, &QLocalSocket::errorOccurred, this, &IpcFrameTransport::on_socket_error);
}

void IpcFrameTransport::set_reconnect_delay_milliseconds(const int delay_ms) {
    reconnect_timer_->setInterval(delay_ms);
}

void IpcFrameTransport::set_auto_reconnect_enabled(const bool enabled) {
    auto_reconnect_enabled_ = enabled;
    if (!enabled) {
        reconnect_timer_->stop();
    }
}

bool IpcFrameTransport::auto_reconnect_enabled() const noexcept {
    return auto_reconnect_enabled_;
}

bool IpcFrameTransport::is_connected() const noexcept {
    return socket_->state() == QLocalSocket::ConnectedState;
}

void IpcFrameTransport::connect_to_service() {
    reconnect_timer_->stop();
    // Suppress error/disconnect signals while resetting the socket — abort() otherwise
    // reports a false connect_failed and can leave the client permanently offline.
    intentional_disconnect_ = true;
    error_notified_ = true;
    input_.clear();
    expected_frame_bytes_ = 0;
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->disconnectFromServer();
        if (socket_->state() != QLocalSocket::UnconnectedState) {
            socket_->abort();
        }
    }
    intentional_disconnect_ = false;
    error_notified_ = false;
    socket_->connectToServer(pipe_name_, QIODevice::ReadWrite);
    // If the server is missing, Qt may emit error synchronously.
    if (socket_->state() == QLocalSocket::UnconnectedState && !error_notified_) {
        error_notified_ = true;
        emit transport_error(QStringLiteral("service.connect_failed"));
        if (!intentional_disconnect_) {
            schedule_reconnect();
        }
    }
}

void IpcFrameTransport::disconnect_from_service() {
    intentional_disconnect_ = true;
    reconnect_timer_->stop();
    error_notified_ = true;
    input_.clear();
    expected_frame_bytes_ = 0;
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->disconnectFromServer();
        if (socket_->state() != QLocalSocket::UnconnectedState) {
            socket_->abort();
        }
    }
    error_notified_ = false;
}

bool IpcFrameTransport::send_frame(const QByteArray& body) {
    if (!is_connected()) {
        return false;
    }
    if (body.isEmpty() || static_cast<quint32>(body.size()) > kMaximumFrameBytes) {
        return false;
    }
    const auto encoded = frame(body);
    if (socket_->write(encoded) != encoded.size()) {
        return false;
    }
    socket_->flush();
    return true;
}

void IpcFrameTransport::on_connected() {
    input_.clear();
    expected_frame_bytes_ = 0;
    error_notified_ = false;
    emit connected();
}

void IpcFrameTransport::on_disconnected() {
    input_.clear();
    expected_frame_bytes_ = 0;
    // Socket error path already notified the client; avoid a second disconnect+reconnect cycle.
    if (!error_notified_) {
        emit disconnected();
    }
    error_notified_ = false;
    if (!intentional_disconnect_) {
        schedule_reconnect();
    }
}

void IpcFrameTransport::on_ready_read() {
    input_.append(socket_->readAll());
    consume_frames();
}

void IpcFrameTransport::on_socket_error() {
    if (socket_->state() == QLocalSocket::ConnectedState) {
        return;
    }
    if (error_notified_) {
        return;
    }
    error_notified_ = true;
    emit transport_error(QStringLiteral("service.connect_failed"));
    if (!intentional_disconnect_) {
        schedule_reconnect();
    }
}

void IpcFrameTransport::consume_frames() {
    for (;;) {
        if (expected_frame_bytes_ == 0) {
            if (input_.size() < 4) {
                return;
            }
            expected_frame_bytes_ = decode_length(input_);
            input_.remove(0, 4);
            if (expected_frame_bytes_ == 0 || expected_frame_bytes_ > kMaximumFrameBytes) {
                fail_protocol();
                return;
            }
        }
        if (input_.size() < static_cast<int>(expected_frame_bytes_)) {
            return;
        }
        const auto body = input_.left(static_cast<int>(expected_frame_bytes_));
        input_.remove(0, static_cast<int>(expected_frame_bytes_));
        expected_frame_bytes_ = 0;
        emit frame_received(body);
    }
}

void IpcFrameTransport::schedule_reconnect() {
    if (!auto_reconnect_enabled_ || intentional_disconnect_) {
        return;
    }
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->start();
    }
}

void IpcFrameTransport::ensure_reconnect_scheduled() {
    intentional_disconnect_ = false;
    auto_reconnect_enabled_ = true;
    if (is_connected()) {
        return;
    }
    if (socket_->state() == QLocalSocket::ConnectingState) {
        return;
    }
    // Prefer an immediate attempt; fall back to the timer if already in flight.
    if (!reconnect_timer_->isActive()) {
        // Zero-delay single-shot: coalesce with the event loop instead of re-entering connect.
        reconnect_timer_->setInterval(0);
        reconnect_timer_->start();
        reconnect_timer_->setInterval(kDefaultReconnectDelayMilliseconds);
    }
}

void IpcFrameTransport::fail_protocol() {
    emit transport_error(QStringLiteral("service.protocol_invalid"));
    socket_->abort();
}

} // namespace aegra::desktop
