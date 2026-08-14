#include "client/ipc_frame_transport.h"

#include <QLocalSocket>
#include <QMetaObject>
#include <QThread>
#include <QTimer>

#include <algorithm>

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

IpcFrameTransportWorker::IpcFrameTransportWorker(QString pipe_name)
    : pipe_name_(std::move(pipe_name)) {}

void IpcFrameTransportWorker::initialize() {
    socket_ = new QLocalSocket(this);
    reconnect_timer_ = new QTimer(this);
    reconnect_timer_->setSingleShot(true);
    reconnect_timer_->setInterval(kDefaultReconnectDelayMilliseconds);
    connect(reconnect_timer_, &QTimer::timeout, this,
            &IpcFrameTransportWorker::connect_to_service);
    connect(socket_, &QLocalSocket::connected, this, &IpcFrameTransportWorker::on_connected);
    connect(socket_, &QLocalSocket::disconnected, this,
            &IpcFrameTransportWorker::on_disconnected);
    connect(socket_, &QLocalSocket::readyRead, this, &IpcFrameTransportWorker::on_ready_read);
    connect(socket_, &QLocalSocket::errorOccurred, this,
            &IpcFrameTransportWorker::on_socket_error);
}

void IpcFrameTransportWorker::shutdown() {
    intentional_disconnect_ = true;
    reconnect_timer_->stop();
    socket_->abort();
}

void IpcFrameTransportWorker::set_reconnect_delay_milliseconds(const int delay_ms) {
    reconnect_delay_ms_ = delay_ms > 0 ? delay_ms : kDefaultReconnectDelayMilliseconds;
    next_reconnect_delay_ms_ = reconnect_delay_ms_;
    reconnect_timer_->setInterval(reconnect_delay_ms_);
}

void IpcFrameTransportWorker::set_auto_reconnect_enabled(const bool enabled) {
    auto_reconnect_enabled_ = enabled;
    if (!enabled) {
        reconnect_timer_->stop();
    }
}

void IpcFrameTransportWorker::connect_to_service() {
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

void IpcFrameTransportWorker::disconnect_from_service() {
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

void IpcFrameTransportWorker::send_frame(const QByteArray& body) {
    if (socket_->state() != QLocalSocket::ConnectedState) {
        emit transport_error(QStringLiteral("service.send_failed"));
        return;
    }
    if (body.isEmpty() || static_cast<quint32>(body.size()) > kMaximumFrameBytes) {
        emit transport_error(QStringLiteral("service.send_failed"));
        return;
    }
    const auto encoded = frame(body);
    if (socket_->write(encoded) != encoded.size()) {
        emit transport_error(QStringLiteral("service.send_failed"));
        return;
    }
    socket_->flush();
}

void IpcFrameTransportWorker::on_connected() {
    input_.clear();
    expected_frame_bytes_ = 0;
    error_notified_ = false;
    next_reconnect_delay_ms_ = reconnect_delay_ms_;
    reconnect_timer_->setInterval(reconnect_delay_ms_);
    emit connected();
}

void IpcFrameTransportWorker::on_disconnected() {
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

void IpcFrameTransportWorker::on_ready_read() {
    input_.append(socket_->readAll());
    consume_frames();
}

void IpcFrameTransportWorker::on_socket_error() {
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

void IpcFrameTransportWorker::consume_frames() {
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

void IpcFrameTransportWorker::schedule_reconnect() {
    if (!auto_reconnect_enabled_ || intentional_disconnect_) {
        return;
    }
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->setInterval(next_reconnect_delay_ms_);
        reconnect_timer_->start();
    }
}

void IpcFrameTransportWorker::schedule_reconnect_with_backoff() {
    intentional_disconnect_ = false;
    auto_reconnect_enabled_ = true;
    if (socket_->state() == QLocalSocket::ConnectedState ||
        socket_->state() == QLocalSocket::ConnectingState) {
        return;
    }
    if (reconnect_timer_->isActive()) {
        return;
    }
    reconnect_timer_->setInterval(next_reconnect_delay_ms_);
    reconnect_timer_->start();
    // Exponential backoff for the next failure (cap at 30s).
    next_reconnect_delay_ms_ =
        (std::min)(kMaximumReconnectDelayMilliseconds,
                   (std::max)(reconnect_delay_ms_, next_reconnect_delay_ms_ * 2));
}

void IpcFrameTransportWorker::ensure_reconnect_scheduled() {
    intentional_disconnect_ = false;
    auto_reconnect_enabled_ = true;
    if (socket_->state() == QLocalSocket::ConnectedState) {
        return;
    }
    if (socket_->state() == QLocalSocket::ConnectingState) {
        return;
    }
    // Use current backoff delay — never zero-delay reconnect storms.
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->setInterval(next_reconnect_delay_ms_);
        reconnect_timer_->start();
        next_reconnect_delay_ms_ =
            (std::min)(kMaximumReconnectDelayMilliseconds,
                       (std::max)(reconnect_delay_ms_, next_reconnect_delay_ms_ * 2));
    }
}

void IpcFrameTransportWorker::fail_protocol() {
    emit transport_error(QStringLiteral("service.protocol_invalid"));
    socket_->abort();
}

IpcFrameTransport::IpcFrameTransport(QString pipe_name, QObject* parent)
    : QObject(parent), thread_(new QThread(this)),
      worker_(new IpcFrameTransportWorker(std::move(pipe_name))) {
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::started, worker_, &IpcFrameTransportWorker::initialize);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    connect(worker_, &IpcFrameTransportWorker::connected, this, [this] {
        connected_.store(true);
        emit connected();
    });
    connect(worker_, &IpcFrameTransportWorker::disconnected, this, [this] {
        connected_.store(false);
        emit disconnected();
    });
    connect(worker_, &IpcFrameTransportWorker::frame_received, this,
            &IpcFrameTransport::frame_received);
    connect(worker_, &IpcFrameTransportWorker::transport_error, this,
            &IpcFrameTransport::transport_error);
    thread_->start();
}

IpcFrameTransport::~IpcFrameTransport() {
    if (!thread_->isRunning()) {
        return;
    }
    QMetaObject::invokeMethod(worker_, &IpcFrameTransportWorker::shutdown,
                              Qt::BlockingQueuedConnection);
    thread_->quit();
    thread_->wait();
}

void IpcFrameTransport::invoke_worker(const char* method) {
    QMetaObject::invokeMethod(worker_, method, Qt::QueuedConnection);
}

void IpcFrameTransport::set_reconnect_delay_milliseconds(const int delay_ms) {
    QMetaObject::invokeMethod(worker_, [worker = worker_, delay_ms] {
        worker->set_reconnect_delay_milliseconds(delay_ms);
    });
}

void IpcFrameTransport::set_auto_reconnect_enabled(const bool enabled) {
    auto_reconnect_enabled_.store(enabled);
    QMetaObject::invokeMethod(worker_, [worker = worker_, enabled] {
        worker->set_auto_reconnect_enabled(enabled);
    });
}

bool IpcFrameTransport::auto_reconnect_enabled() const noexcept {
    return auto_reconnect_enabled_.load();
}

bool IpcFrameTransport::is_connected() const noexcept {
    return connected_.load();
}

void IpcFrameTransport::connect_to_service() {
    invoke_worker("connect_to_service");
}

void IpcFrameTransport::disconnect_from_service() {
    connected_.store(false);
    invoke_worker("disconnect_from_service");
}

void IpcFrameTransport::ensure_reconnect_scheduled() {
    auto_reconnect_enabled_.store(true);
    invoke_worker("ensure_reconnect_scheduled");
}

void IpcFrameTransport::schedule_reconnect_with_backoff() {
    auto_reconnect_enabled_.store(true);
    invoke_worker("schedule_reconnect_with_backoff");
}

bool IpcFrameTransport::send_frame(const QByteArray& body) {
    if (!is_connected() || body.isEmpty() ||
        static_cast<quint32>(body.size()) > kMaximumFrameBytes) {
        return false;
    }
    QMetaObject::invokeMethod(worker_, [worker = worker_, body] { worker->send_frame(body); });
    return true;
}

} // namespace aegra::desktop
