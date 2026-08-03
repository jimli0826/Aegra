#include "service_client.h"

#include <QAbstractSocket>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <array>
#include <initializer_list>
#include <utility>

namespace {

constexpr auto kServicePipeName = "aegra-service-control";
constexpr quint32 kMaximumFrameBytes = 64U * 1024U;
constexpr int kReconnectDelayMilliseconds = 2'000;
constexpr qsizetype kMaximumVersionCharacters = 64;
constexpr qsizetype kMaximumCapabilities = 64;
constexpr qsizetype kMaximumCapabilityCharacters = 64;

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

[[nodiscard]] bool has_exact_keys(const QJsonObject& object,
                                  const std::initializer_list<const char*> keys) {
    if (object.size() != static_cast<int>(keys.size())) {
        return false;
    }
    for (const auto* key : keys) {
        if (!object.contains(QLatin1String(key))) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_stable_code(const QString& value, const qsizetype maximum_characters) {
    if (value.isEmpty() || value.size() > maximum_characters) {
        return false;
    }
    return std::all_of(value.cbegin(), value.cend(), [](const QChar character) {
        const auto code = character.unicode();
        return (code >= 'a' && code <= 'z') || (code >= '0' && code <= '9') || code == '.' ||
               code == '_' || code == '-';
    });
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
    const QJsonObject request{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("request_id"), request_id_},
        {QStringLiteral("kind"), 1},
    };
    const auto body = QJsonDocument(request).toJson(QJsonDocument::Compact);
    socket_->write(frame(body));
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
    QJsonParseError parse_error{};
    const auto document = QJsonDocument::fromJson(frame_body, &parse_error);
    if (parse_error.error != QJsonParseError::NoError || !document.isObject()) {
        return false;
    }
    const auto root = document.object();
    if (!has_exact_keys(root, {"schema_version", "request_id", "kind", "boundary_error_code",
                               "message_code", "service"}) ||
        root.value(QStringLiteral("schema_version")).toInt(-1) != 1 ||
        root.value(QStringLiteral("request_id")).toString() != request_id_ ||
        root.value(QStringLiteral("kind")).toInt(-1) != 1 ||
        root.value(QStringLiteral("boundary_error_code")).toInt(-1) != 0 ||
        root.value(QStringLiteral("message_code")).toString() != QStringLiteral("service.ready") ||
        !root.value(QStringLiteral("service")).isObject()) {
        return false;
    }
    return apply_service_info(root.value(QStringLiteral("service")).toObject());
}

bool ServiceClient::apply_service_info(const QJsonObject& service) {
    if (!has_exact_keys(service, {"api_version", "state", "service_version", "capabilities"}) ||
        service.value(QStringLiteral("api_version")).toInt(-1) != 1 ||
        service.value(QStringLiteral("state")).toInt(-1) != 2 ||
        !service.value(QStringLiteral("service_version")).isString() ||
        !service.value(QStringLiteral("capabilities")).isArray()) {
        return false;
    }
    const auto capability_values = service.value(QStringLiteral("capabilities")).toArray();
    if (capability_values.isEmpty() || capability_values.size() > kMaximumCapabilities) {
        return false;
    }
    QStringList capabilities;
    for (const auto value : capability_values) {
        if (!value.isString() || !is_stable_code(value.toString(), kMaximumCapabilityCharacters)) {
            return false;
        }
        capabilities.push_back(value.toString());
    }
    const auto service_version = service.value(QStringLiteral("service_version")).toString();
    if (service_version.isEmpty() || service_version.size() > kMaximumVersionCharacters ||
        !std::is_sorted(capabilities.cbegin(), capabilities.cend()) ||
        std::adjacent_find(capabilities.cbegin(), capabilities.cend()) != capabilities.cend()) {
        return false;
    }
    service_version_ = service_version;
    api_version_ = static_cast<quint32>(service.value(QStringLiteral("api_version")).toInt());
    capabilities_ = std::move(capabilities);
    set_state(State::kReady);
    return true;
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
    }
    emit stateChanged();
}
