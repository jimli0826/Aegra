#include "service_client.h"

#include <QByteArray>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QThread>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <utility>

namespace {

constexpr auto kPipeName = "aegra-service-control";
constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFirstFileUuid = "11111111-2222-4333-8444-555555555555";
constexpr auto kSecondFileUuid = "22222222-3333-4444-8555-666666666666";
constexpr int kShortTimeoutMilliseconds = 3'000;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool wait_until(const std::function<bool()>& predicate, const int timeout_ms) {
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

quint32 decode_length(const QByteArray& input) {
    return static_cast<quint32>(static_cast<unsigned char>(input[0])) |
           (static_cast<quint32>(static_cast<unsigned char>(input[1])) << 8U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[2])) << 16U) |
           (static_cast<quint32>(static_cast<unsigned char>(input[3])) << 24U);
}

QByteArray frame(const QByteArray& body) {
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

QByteArray receive_frame(QLocalSocket& socket, const int timeout_ms) {
    QByteArray input;
    quint32 expected = 0;
    const auto complete = [&] {
        input.append(socket.readAll());
        if (expected == 0 && input.size() >= 4) {
            expected = decode_length(input);
            input.remove(0, 4);
        }
        return expected > 0 && input.size() >= static_cast<int>(expected);
    };
    return wait_until(complete, timeout_ms) ? input.left(static_cast<int>(expected)) : QByteArray{};
}

QJsonObject receive_object(QLocalSocket& socket, const int timeout_ms) {
    const auto document = QJsonDocument::fromJson(receive_frame(socket, timeout_ms));
    return document.isObject() ? document.object() : QJsonObject{};
}

QLocalSocket* accept_client(QLocalServer& server, const int timeout_ms) {
    if (!wait_until([&] { return server.hasPendingConnections(); }, timeout_ms)) {
        return nullptr;
    }
    return server.nextPendingConnection();
}

bool send_object(QLocalSocket& socket, const QJsonObject& object) {
    const auto encoded = frame(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return socket.write(encoded) == encoded.size() && socket.waitForBytesWritten(1'000);
}

QJsonObject response(const QString& request_id, const qint64 kind, const qint64 error,
                     QString message_code, QJsonValue service, QJsonValue recovery_points) {
    return {{QStringLiteral("schema_version"), 2},
            {QStringLiteral("request_id"), request_id},
            {QStringLiteral("kind"), kind},
            {QStringLiteral("boundary_error_code"), error},
            {QStringLiteral("message_code"), std::move(message_code)},
            {QStringLiteral("service"), std::move(service)},
            {QStringLiteral("recovery_points"), std::move(recovery_points)}};
}

bool send_service_info(QLocalSocket& socket, const QString& request_id,
                       QJsonArray capabilities = {QStringLiteral("repository.list"),
                                                  QStringLiteral("service.info")}) {
    const QJsonObject service{{QStringLiteral("api_version"), 2},
                              {QStringLiteral("state"), 2},
                              {QStringLiteral("service_version"), QStringLiteral("0.1.0")},
                              {QStringLiteral("capabilities"), std::move(capabilities)}};
    return send_object(socket, response(request_id, 1, 0, QStringLiteral("service.ready"), service,
                                        QJsonValue(QJsonValue::Null)));
}

QJsonObject recovery_point(const QString& file_uuid, const std::optional<QString>& parent_uuid,
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

bool send_catalog_page(QLocalSocket& socket, const QString& request_id, QJsonArray items,
                       const std::optional<QString>& token) {
    const QJsonObject page{{QStringLiteral("state"), 2},
                           {QStringLiteral("repository_uuid"), QLatin1String(kRepositoryUuid)},
                           {QStringLiteral("items"), std::move(items)},
                           {QStringLiteral("continuation_token"),
                            token ? QJsonValue(*token) : QJsonValue(QJsonValue::Null)}};
    return send_object(socket,
                       response(request_id, 3, 0, QStringLiteral("repository.catalog_ready"),
                                QJsonValue(QJsonValue::Null), page));
}

bool send_not_configured(QLocalSocket& socket, const QString& request_id) {
    const QJsonObject page{{QStringLiteral("state"), 1},
                           {QStringLiteral("repository_uuid"), QString{}},
                           {QStringLiteral("items"), QJsonArray{}},
                           {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}};
    return send_object(socket,
                       response(request_id, 3, 0, QStringLiteral("repository.not_configured"),
                                QJsonValue(QJsonValue::Null), page));
}

bool send_repository_failure(QLocalSocket& socket, const QString& request_id) {
    return send_object(socket,
                       response(request_id, 2, 5, QStringLiteral("repository.query_failed"),
                                QJsonValue(QJsonValue::Null), QJsonValue(QJsonValue::Null)));
}

bool valid_request(const QJsonObject& request, const qint64 kind) {
    return request.value(QStringLiteral("schema_version")).toInteger() == 2 &&
           request.value(QStringLiteral("kind")).toInteger() == kind &&
           !request.value(QStringLiteral("request_id")).toString().isEmpty();
}

bool begin_repository_query(QLocalServer& server, ServiceClient& client, QLocalSocket*& socket,
                            QJsonObject& list_request) {
    socket = accept_client(server, kShortTimeoutMilliseconds);
    if (socket == nullptr) {
        return false;
    }
    const auto info_request = receive_object(*socket, kShortTimeoutMilliseconds);
    if (!valid_request(info_request, 1) ||
        !send_service_info(*socket, info_request.value(QStringLiteral("request_id")).toString())) {
        return false;
    }
    list_request = receive_object(*socket, kShortTimeoutMilliseconds);
    return wait_until([&] { return client.connected(); }, kShortTimeoutMilliseconds) &&
           valid_request(list_request, 2);
}

bool first_page_request_is_valid(const QJsonObject& request) {
    const auto list = request.value(QStringLiteral("repository_list")).toObject();
    return request.size() == 4 && list.size() == 2 &&
           list.value(QStringLiteral("maximum_results")).toInteger() == 100 &&
           list.value(QStringLiteral("continuation_token")).isNull();
}

bool verify_reconnect(QLocalServer& server, ServiceClient& client, QLocalSocket& socket) {
    socket.abort();
    bool passed =
        expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds) &&
                   client.recoveryPoints().isEmpty(),
               "disconnect clears repository state");
    auto* reconnected = accept_client(server, 3'500);
    if (!expect(reconnected != nullptr, "desktop automatically reconnects")) {
        return false;
    }
    const auto info_request = receive_object(*reconnected, kShortTimeoutMilliseconds);
    passed &= expect(send_service_info(*reconnected,
                                       info_request.value(QStringLiteral("request_id")).toString()),
                     "reconnected service sends info");
    const auto list_request = receive_object(*reconnected, kShortTimeoutMilliseconds);
    passed &= expect(send_not_configured(
                         *reconnected, list_request.value(QStringLiteral("request_id")).toString()),
                     "reconnected service reports no repository");
    passed &= expect(wait_until([&] { return client.connected() && !client.repositoryLoading(); },
                                kShortTimeoutMilliseconds) &&
                         !client.repositoryConfigured() && client.recoveryPoints().isEmpty(),
                     "reconnect starts a fresh repository query");
    reconnected->abort();
    return passed;
}

bool test_pagination_and_reconnect() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "pagination listener starts")) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject first_request;
    if (!expect(begin_repository_query(server, client, socket, first_request),
                "desktop handshakes and requests repository") ||
        !expect(first_page_request_is_valid(first_request),
                "desktop requests a bounded first page")) {
        return false;
    }
    const QString token = QStringLiteral("catalog/recovery-points/first.entry");
    bool passed = expect(
        send_catalog_page(*socket, first_request.value(QStringLiteral("request_id")).toString(),
                          {recovery_point(QLatin1String(kFirstFileUuid), std::nullopt, 1)}, token),
        "fake service sends the first page");
    const auto second_request = receive_object(*socket, kShortTimeoutMilliseconds);
    const auto second_list = second_request.value(QStringLiteral("repository_list")).toObject();
    passed &=
        expect(valid_request(second_request, 2) &&
                   second_list.value(QStringLiteral("continuation_token")).toString() == token,
               "desktop forwards the opaque continuation token");
    passed &= expect(
        send_catalog_page(*socket, second_request.value(QStringLiteral("request_id")).toString(),
                          {recovery_point(QLatin1String(kSecondFileUuid),
                                          QString(QLatin1String(kFirstFileUuid)), 2)},
                          std::nullopt),
        "fake service sends the final page");
    passed &=
        expect(wait_until([&] { return !client.repositoryLoading(); }, kShortTimeoutMilliseconds) &&
                   client.repositoryConfigured() && client.recoveryPoints().size() == 2 &&
                   client.repositoryUuid() == QLatin1String(kRepositoryUuid),
               "desktop atomically publishes the merged catalog");
    passed &= verify_reconnect(server, client, *socket);
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_repository_failure_keeps_connection() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "failure listener starts")) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject request;
    if (!expect(begin_repository_query(server, client, socket, request),
                "failure test reaches repository query")) {
        return false;
    }
    bool passed = expect(send_repository_failure(
                             *socket, request.value(QStringLiteral("request_id")).toString()),
                         "fake service sends repository failure") &&
                  expect(wait_until([&] { return !client.repositoryErrorText().isEmpty(); },
                                    kShortTimeoutMilliseconds) &&
                             client.connected() && !client.repositoryLoading(),
                         "repository failure does not disconnect the service");
    client.refreshRepository();
    const auto refresh_request = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &=
        expect(valid_request(refresh_request, 2) && first_page_request_is_valid(refresh_request),
               "manual refresh starts a new bounded repository query");
    client.refreshRepository();
    passed &= expect(send_not_configured(
                         *socket, refresh_request.value(QStringLiteral("request_id")).toString()),
                     "manual refresh receives a repository response");
    passed &= expect(wait_until(
                         [&] {
                             return client.repositoryErrorText().isEmpty() && client.connected() &&
                                    !client.repositoryLoading();
                         },
                         kShortTimeoutMilliseconds),
                     "successful manual refresh clears the repository error");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_cross_page_order_violation() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "ordering listener starts")) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject request;
    if (!expect(begin_repository_query(server, client, socket, request),
                "ordering test reaches repository query")) {
        return false;
    }
    const QString token = QStringLiteral("catalog/recovery-points/second.entry");
    bool passed = expect(
        send_catalog_page(*socket, request.value(QStringLiteral("request_id")).toString(),
                          {recovery_point(QLatin1String(kSecondFileUuid), std::nullopt, 1)}, token),
        "fake service sends ordered first page");
    const auto second_request = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &=
        expect(send_catalog_page(
                   *socket, second_request.value(QStringLiteral("request_id")).toString(),
                   {recovery_point(QLatin1String(kFirstFileUuid), std::nullopt, 1)}, std::nullopt),
               "fake service sends cross-page disorder");
    passed &= expect(
        wait_until([&] { return !client.errorText().isEmpty(); }, kShortTimeoutMilliseconds) &&
            !client.connected(),
        "desktop rejects cross-page UUID disorder");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_invalid_service_info() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "validation listener starts")) {
        return false;
    }
    ServiceClient client;
    auto* socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(socket != nullptr, "validation client connects")) {
        return false;
    }
    const auto request = receive_object(*socket, kShortTimeoutMilliseconds);
    const QJsonArray unsorted{QStringLiteral("service.info"), QStringLiteral("repository.list")};
    bool passed =
        expect(send_service_info(*socket, request.value(QStringLiteral("request_id")).toString(),
                                 unsorted),
               "fake service sends invalid capability order") &&
        expect(
            wait_until([&] { return !client.errorText().isEmpty(); }, kShortTimeoutMilliseconds) &&
                !client.connected(),
            "desktop rejects invalid service information");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

} // namespace

int main(int argument_count, char* arguments[]) noexcept {
    QCoreApplication application(argument_count, arguments);
    try {
        const auto pagination = test_pagination_and_reconnect();
        const auto failure = test_repository_failure_keeps_connection();
        const auto ordering = test_cross_page_order_violation();
        const auto service_info = test_invalid_service_info();
        return pagination && failure && ordering && service_info ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
