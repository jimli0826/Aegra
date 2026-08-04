#include "client/service_client.h"
#include "desktop_client_test_support.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

using aegra::desktop::ServiceClient;
using namespace aegra::desktop::test_support;

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
    const auto payload = request.value(QStringLiteral("payload")).toObject();
    const auto list = payload.value(QStringLiteral("page")).toObject();
    return request.size() == 6 && payload.size() == 2 &&
           payload.value(QStringLiteral("repository_connection_id")).isNull() && list.size() == 2 &&
           list.value(QStringLiteral("maximum_results")).toInteger() == 100 &&
           list.value(QStringLiteral("continuation_token")).isNull();
}

bool verify_reconnect(QLocalServer& server, ServiceClient& client, QLocalSocket& socket) {
    socket.abort();
    bool passed =
        expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds) &&
                   client.recoveryPointCount() == 0,
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
                         !client.repositoryConfigured() && client.recoveryPointCount() == 0,
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
    const auto second_list = second_request.value(QStringLiteral("payload"))
                                 .toObject()
                                 .value(QStringLiteral("page"))
                                 .toObject();
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
                   client.repositoryConfigured() && client.recoveryPointCount() == 2 &&
                   client.repositoryUuid() == QLatin1String(kRepositoryUuid),
               "desktop atomically publishes the merged catalog");
    const auto* model = client.recoveryPoints();
    passed &= expect(model != nullptr && model->rowCount() == 2, "recovery point model owns rows");
    if (model != nullptr && model->rowCount() == 2) {
        const auto first = model->index(0, 0);
        passed &= expect(
            model->data(first, aegra::desktop::RecoveryPointModel::FileUuidRole).toString() ==
                QLatin1String(kFirstFileUuid),
            "model exposes structured file uuid");
        passed &= expect(!model->data(first, aegra::desktop::RecoveryPointModel::BackupTypeTextRole)
                              .toString()
                              .isEmpty(),
                         "model exposes localized backup type text");
    }
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

void drain_pending_connections(QLocalServer& server) {
    while (server.hasPendingConnections()) {
        auto* pending = server.nextPendingConnection();
        if (pending != nullptr) {
            pending->abort();
            pending->deleteLater();
        }
    }
}

bool test_splash_failure_and_retry() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    bool passed = expect(client.splashVisible(), "splash visible before ready");
    auto* socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(socket != nullptr, "initial connection accepted")) {
        return false;
    }
    const auto info = receive_object(*socket, kShortTimeoutMilliseconds);
    const QJsonArray bad_caps{QStringLiteral("service.info")};
    passed &=
        expect(valid_request(info, 1) &&
                   send_service_info(*socket, info.value(QStringLiteral("request_id")).toString(),
                                     bad_caps),
               "invalid capabilities sent");
    passed &= expect(wait_until([&] { return client.splashVisible() && !client.connected(); },
                                kShortTimeoutMilliseconds),
                     "splash remains for failed handshake");
    socket->abort();
    if (wait_until([&] { return server.hasPendingConnections(); }, 500)) {
        drain_pending_connections(server);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    drain_pending_connections(server);

    client.reconnect();
    auto* retry_socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(retry_socket != nullptr, "second retry accepted")) {
        return false;
    }
    const auto retry_info = receive_object(*retry_socket, kShortTimeoutMilliseconds);
    const QJsonArray capabilities{QStringLiteral("repository.list"),
                                  QStringLiteral("service.info")};
    passed &=
        expect(valid_request(retry_info, 1) &&
                   send_service_info(*retry_socket,
                                     retry_info.value(QStringLiteral("request_id")).toString(),
                                     capabilities),
               "valid capabilities sent on retry");
    const auto list_request = receive_object(*retry_socket, kShortTimeoutMilliseconds);
    passed &=
        expect(valid_request(list_request, 2) &&
                   send_not_configured(*retry_socket,
                                       list_request.value(QStringLiteral("request_id")).toString()),
               "repository response sent after retry");
    passed &= expect(wait_until([&] { return client.connected() && !client.splashVisible(); },
                                kShortTimeoutMilliseconds),
                     "splash exits after successful retry");
    retry_socket->abort();
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
        const bool passed = test_pagination_and_reconnect() &&
                            test_repository_failure_keeps_connection() &&
                            test_cross_page_order_violation() && test_splash_failure_and_retry() &&
                            test_invalid_service_info();
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
