#include "service_client.h"

#include "service_protocol.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <QUuid>

#include <utility>

namespace {

constexpr auto kServicePipeName = "aegra-service-control";
constexpr quint32 kMaximumFrameBytes = 64U * 1024U;
constexpr int kReconnectDelayMilliseconds = 2'000;
constexpr qsizetype kMaximumRecoveryPoints = 10'000;

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

ServiceClient::ServiceClient(QObject* parent)
    : QObject(parent), socket_(new QLocalSocket(this)), reconnect_timer_(new QTimer(this)) {
    reconnect_timer_->setSingleShot(true);
    reconnect_timer_->setInterval(kReconnectDelayMilliseconds);
    connect(reconnect_timer_, &QTimer::timeout, this, &ServiceClient::reconnect);
    connect(socket_, &QLocalSocket::connected, this, &ServiceClient::on_connected);
    connect(socket_, &QLocalSocket::disconnected, this, &ServiceClient::on_disconnected);
    connect(socket_, &QLocalSocket::readyRead, this, &ServiceClient::on_ready_read);
    connect(socket_, &QLocalSocket::errorOccurred, this, &ServiceClient::on_socket_error);
    QTimer::singleShot(0, this, &ServiceClient::reconnect);
}

bool ServiceClient::connected() const noexcept { return state_ == State::kReady; }

QString ServiceClient::statusText() const {
    switch (state_) {
    case State::kDisconnected:
        return QStringLiteral("未连接");
    case State::kConnecting:
        return QStringLiteral("连接中");
    case State::kReady:
        return QStringLiteral("运行中");
    }
    return QStringLiteral("未知");
}

QString ServiceClient::serviceVersion() const { return service_version_; }

quint32 ServiceClient::apiVersion() const noexcept { return api_version_; }

QStringList ServiceClient::capabilities() const { return capabilities_; }

QString ServiceClient::errorText() const { return error_text_; }

bool ServiceClient::repositoryConfigured() const noexcept { return repository_configured_; }

bool ServiceClient::repositoryLoading() const noexcept { return repository_loading_; }

QString ServiceClient::repositoryUuid() const { return repository_uuid_; }

QString ServiceClient::repositoryStatusText() const {
    if (!connected()) {
        return QStringLiteral("等待 Service");
    }
    if (repository_loading_) {
        return QStringLiteral("正在读取目录");
    }
    if (!repository_error_text_.isEmpty()) {
        return QStringLiteral("目录读取失败");
    }
    return repository_configured_ ? QStringLiteral("目录可用") : QStringLiteral("未配置");
}

QString ServiceClient::repositoryErrorText() const { return repository_error_text_; }

QVariantList ServiceClient::recoveryPoints() const { return recovery_points_; }

void ServiceClient::reconnect() {
    reconnect_timer_->stop();
    input_.clear();
    expected_frame_bytes_ = 0;
    if (socket_->state() != QLocalSocket::UnconnectedState) {
        socket_->abort();
    }
    set_state(State::kConnecting);
    socket_->connectToServer(QLatin1String(kServicePipeName), QIODevice::ReadWrite);
}

void ServiceClient::refreshRepository() {
    if (state_ != State::kReady || pending_request_ != PendingRequest::kNone ||
        repository_loading_) {
        return;
    }
    start_repository_query();
}

void ServiceClient::on_connected() {
    input_.clear();
    expected_frame_bytes_ = 0;
    send_service_info_request();
}

void ServiceClient::on_disconnected() {
    if (state_ != State::kDisconnected) {
        set_state(State::kDisconnected, QStringLiteral("Service 连接已断开"));
    }
    schedule_reconnect();
}

void ServiceClient::on_ready_read() {
    input_.append(socket_->readAll());
    consume_frames();
}

void ServiceClient::on_socket_error() {
    set_state(State::kDisconnected, QStringLiteral("无法连接到 Service"));
    schedule_reconnect();
}

void ServiceClient::send_service_info_request() {
    request_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_request_ = PendingRequest::kServiceInfo;
    socket_->write(frame(aegra::desktop::encode_service_info_request(request_id_)));
    socket_->flush();
}

void ServiceClient::start_repository_query() {
    reset_repository();
    repository_loading_ = true;
    emit repositoryChanged();
    send_recovery_point_request(std::nullopt);
}

void ServiceClient::send_recovery_point_request(const std::optional<QString>& token) {
    request_id_ = QUuid::createUuid().toString(QUuid::WithoutBraces);
    pending_request_ = PendingRequest::kRecoveryPoints;
    requested_token_ = token;
    socket_->write(
        frame(aegra::desktop::encode_recovery_point_request(request_id_, requested_token_)));
    socket_->flush();
}

void ServiceClient::consume_frames() {
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
        if (!apply_response(body)) {
            fail_protocol();
            return;
        }
    }
}

bool ServiceClient::apply_response(const QByteArray& frame_body) {
    QJsonObject root;
    if (!aegra::desktop::parse_response_root(frame_body, request_id_, root)) {
        return false;
    }
    switch (pending_request_) {
    case PendingRequest::kServiceInfo:
        return apply_service_info(root);
    case PendingRequest::kRecoveryPoints:
        if (aegra::desktop::is_repository_failure_response(root)) {
            finish_repository_failure();
            return true;
        }
        return apply_recovery_point_page(root);
    case PendingRequest::kNone:
        return false;
    }
    return false;
}

bool ServiceClient::apply_service_info(const QJsonObject& root) {
    aegra::desktop::ServiceInfo service;
    if (!aegra::desktop::parse_service_info_response(root, service) ||
        !service.capabilities.contains(QStringLiteral("repository.list"))) {
        return false;
    }
    service_version_ = std::move(service.version);
    api_version_ = aegra::desktop::kServiceApiVersion;
    capabilities_ = std::move(service.capabilities);
    pending_request_ = PendingRequest::kNone;
    set_state(State::kReady);
    start_repository_query();
    return true;
}

bool ServiceClient::apply_recovery_point_page(const QJsonObject& root) {
    aegra::desktop::RecoveryPointPage page;
    if (!aegra::desktop::parse_recovery_point_response(root, page)) {
        return false;
    }
    if (!page.configured) {
        if (requested_token_ || !pending_recovery_points_.isEmpty()) {
            return false;
        }
        repository_loading_ = false;
        pending_request_ = PendingRequest::kNone;
        emit repositoryChanged();
        return true;
    }
    if ((!repository_uuid_.isEmpty() && page.repository_uuid != repository_uuid_) ||
        (page.continuation_token && page.continuation_token == requested_token_) ||
        pending_recovery_points_.size() + page.items.size() > kMaximumRecoveryPoints) {
        return false;
    }
    repository_uuid_ = page.repository_uuid;
    for (auto& item : page.items) {
        const auto file_uuid = item.toMap().value(QStringLiteral("fileUuid")).toString();
        if (!last_file_uuid_.isEmpty() && file_uuid <= last_file_uuid_) {
            return false;
        }
        last_file_uuid_ = file_uuid;
        pending_recovery_points_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        send_recovery_point_request(page.continuation_token);
        return true;
    }
    recovery_points_ = std::move(pending_recovery_points_);
    repository_configured_ = true;
    repository_loading_ = false;
    pending_request_ = PendingRequest::kNone;
    requested_token_.reset();
    emit repositoryChanged();
    return true;
}

void ServiceClient::finish_repository_failure() {
    repository_uuid_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_loading_ = false;
    pending_request_ = PendingRequest::kNone;
    repository_error_text_ = QStringLiteral("无法读取 Repository 目录");
    emit repositoryChanged();
}

void ServiceClient::reset_repository() {
    repository_uuid_.clear();
    repository_error_text_.clear();
    recovery_points_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_configured_ = false;
    repository_loading_ = false;
    emit repositoryChanged();
}

void ServiceClient::fail_protocol() {
    set_state(State::kDisconnected, QStringLiteral("Service 响应无效"));
    socket_->abort();
    schedule_reconnect();
}

void ServiceClient::schedule_reconnect() {
    if (!reconnect_timer_->isActive()) {
        reconnect_timer_->start();
    }
}

void ServiceClient::set_state(const State state, QString error) {
    state_ = state;
    error_text_ = std::move(error);
    if (state != State::kReady) {
        service_version_.clear();
        api_version_ = 0;
        capabilities_.clear();
        request_id_.clear();
        pending_request_ = PendingRequest::kNone;
        reset_repository();
    }
    emit stateChanged();
}
