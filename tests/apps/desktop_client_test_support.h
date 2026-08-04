#pragma once

#include "client/service_client.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>

#include <cstdio>
#include <functional>
#include <optional>
#include <utility>

namespace aegra::desktop::test_support {

inline constexpr auto kPipeName = "aegra-service-control";
inline constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
inline constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
inline constexpr auto kFirstFileUuid = "11111111-2222-4333-8444-555555555555";
inline constexpr auto kSecondFileUuid = "22222222-3333-4444-8555-666666666666";
inline constexpr int kShortTimeoutMilliseconds = 3'000;

inline bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

inline bool wait_until(const std::function<bool()>& predicate, const int timeout_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
        QThread::msleep(5);
    }
    return predicate();
}

inline quint32 decode_length(const QByteArray& input) {
    return static_cast<quint32>(static_cast<unsigned char>(input[0])) |
           (static_cast<quint32>(static_cast<unsigned char>(input[1])) << 8U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[2])) << 16U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[3])) << 24U);
}

inline QByteArray frame(const QByteArray& body) {
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

inline QByteArray& socket_inbox(QLocalSocket& socket) {
    static QHash<QLocalSocket*, QByteArray> inboxes;
    return inboxes[&socket];
}

inline QByteArray receive_frame(QLocalSocket& socket, const int timeout_ms) {
    auto& input = socket_inbox(socket);
    quint32 expected = 0;
    const auto complete = [&] {
        input.append(socket.readAll());
        if (expected == 0) {
            if (input.size() < 4) {
                return false;
            }
            expected = decode_length(input);
            input.remove(0, 4);
        }
        return expected > 0 && input.size() >= static_cast<int>(expected);
    };
    if (!wait_until(complete, timeout_ms)) {
        return {};
    }
    const auto body = input.left(static_cast<int>(expected));
    input.remove(0, static_cast<int>(expected));
    return body;
}

inline QJsonObject receive_object(QLocalSocket& socket, const int timeout_ms) {
    const auto document = QJsonDocument::fromJson(receive_frame(socket, timeout_ms));
    return document.isObject() ? document.object() : QJsonObject{};
}

inline QLocalSocket* accept_client(QLocalServer& server, const int timeout_ms) {
    if (!wait_until([&] { return server.hasPendingConnections(); }, timeout_ms)) {
        return nullptr;
    }
    return server.nextPendingConnection();
}

inline bool send_object(QLocalSocket& socket, const QJsonObject& object) {
    const auto encoded = frame(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return socket.write(encoded) == encoded.size() && socket.waitForBytesWritten(1'000);
}

inline QJsonObject response(const QString& request_id, const qint64 kind, const qint64 request_kind,
                            const qint64 error, QString message_code, QJsonValue payload) {
    return {{QStringLiteral("schema_version"), 3},
            {QStringLiteral("message_type"), 2},
            {QStringLiteral("request_id"), request_id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("request_kind"), request_kind},
            {QStringLiteral("boundary_error_code"), error},
            {QStringLiteral("message_code"), std::move(message_code)},
            {QStringLiteral("message_arguments"), QJsonArray{}},
            {QStringLiteral("payload"), std::move(payload)}};
}

inline bool send_service_info(QLocalSocket& socket, const QString& request_id,
                              QJsonArray capabilities = {QStringLiteral("repository.list"),
                                                         QStringLiteral("service.info")}) {
    const QJsonObject service{{QStringLiteral("minimum_api_version"), 3},
                              {QStringLiteral("api_version"), 3},
                              {QStringLiteral("state"), 2},
                              {QStringLiteral("service_version"), QStringLiteral("0.1.0")},
                              {QStringLiteral("capabilities"), std::move(capabilities)}};
    return send_object(socket,
                       response(request_id, 1, 1, 0, QStringLiteral("service.ready"), service));
}

inline QJsonObject recovery_point(const QString& file_uuid,
                                  const std::optional<QString>& parent_uuid,
                                  const qint64 backup_type) {
    return {{QStringLiteral("file_uuid"), file_uuid},
            {QStringLiteral("backup_set_uuid"), QLatin1String(kSetUuid)},
            {QStringLiteral("parent_uuid"),
             parent_uuid ? QJsonValue(*parent_uuid) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("backup_type"), backup_type},
            {QStringLiteral("chain_state"), 1},
            {QStringLiteral("created_utc_ms"), 1'775'174'400'000LL},
            {QStringLiteral("logical_size_bytes"), 8'589'934'592LL},
            {QStringLiteral("stored_size_bytes"), 4'294'967'296LL},
            {QStringLiteral("source_count"), 1},
            {QStringLiteral("has_sidecar"), backup_type != 1}};
}

inline bool send_catalog_page(QLocalSocket& socket, const QString& request_id, QJsonArray items,
                              const std::optional<QString>& token,
                              const std::optional<QString>& connection_id = std::nullopt) {
    const QJsonObject catalog{{QStringLiteral("state"), 2},
                              {QStringLiteral("repository_uuid"), QLatin1String(kRepositoryUuid)},
                              {QStringLiteral("items"), std::move(items)},
                              {QStringLiteral("continuation_token"),
                               token ? QJsonValue(*token) : QJsonValue(QJsonValue::Null)}};
    const QJsonObject page{
        {QStringLiteral("repository_connection_id"),
         connection_id ? QJsonValue(*connection_id) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("catalog"), catalog}};
    return send_object(
        socket, response(request_id, 1, 2, 0, QStringLiteral("repository.catalog_ready"), page));
}

inline bool send_not_configured(QLocalSocket& socket, const QString& request_id,
                                const std::optional<QString>& connection_id = std::nullopt) {
    const QJsonObject catalog{{QStringLiteral("state"), 1},
                              {QStringLiteral("repository_uuid"), QString{}},
                              {QStringLiteral("items"), QJsonArray{}},
                              {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}};
    const QJsonObject page{
        {QStringLiteral("repository_connection_id"),
         connection_id ? QJsonValue(*connection_id) : QJsonValue(QJsonValue::Null)},
        {QStringLiteral("catalog"), catalog}};
    return send_object(
        socket, response(request_id, 1, 2, 0, QStringLiteral("repository.not_configured"), page));
}

inline bool send_repository_failure(QLocalSocket& socket, const QString& request_id) {
    return send_object(socket,
                       response(request_id, 3, 2, 5, QStringLiteral("repository.query_failed"),
                                QJsonValue(QJsonValue::Null)));
}

inline bool valid_request(const QJsonObject& request, const qint64 kind) {
    return request.value(QStringLiteral("schema_version")).toInteger() == 3 &&
           request.value(QStringLiteral("message_type")).toInteger() == 1 &&
           request.value(QStringLiteral("kind")).toInteger() == kind &&
           request.value(QStringLiteral("idempotency_key")).isNull() &&
           request.value(QStringLiteral("payload")).isObject() &&
           !request.value(QStringLiteral("request_id")).toString().isEmpty();
}

} // namespace aegra::desktop::test_support
