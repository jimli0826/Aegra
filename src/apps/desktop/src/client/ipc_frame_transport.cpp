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

bool IpcFrameTransport::is_connected() const noexcept {
    return socket_->state() == QLocalSocket::ConnectedState;
}

void IpcFrameTransport::connect_to_service() {
    reconnect_timer_->stop();
    intentional_disconnect_ = false;
    input_.clear();
    expected_frame_bytes_ = 0;
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->abort();
    }
    socket_->connectToServer(pipe_name_, QIODevice::ReadWrite);
}

void IpcFrameTransport::disconnect_from_service() {
    intentional_disconnect_ = true;
    reconnect_timer_->stop();
    input_.clear();
    expected_frame_bytes_ = 0;
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->abort();
    }
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
    emit connected();
}

void IpcFrameTransport::on_disconnected() {
    input_.clear();
    expected_frame_bytes_ = 0;
    emit disconnected();
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
    emit transport_error(QStringLiteral("service.connect_failed"));
    emit disconnected();
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
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->start();
    }
}

void IpcFrameTransport::fail_protocol() {
    emit transport_error(QStringLiteral("service.protocol_invalid"));
    socket_->abort();
}

} // namespace aegra::desktop
