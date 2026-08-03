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
#include <utility>

namespace {

constexpr auto kPipeName = "aegra-service-control";
constexpr int kShortTimeoutMilliseconds = 1'000;

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

QLocalSocket* accept_client(QLocalServer& server, const int timeout_ms) {
    if (!wait_until([&] { return server.hasPendingConnections(); }, timeout_ms)) {
        return nullptr;
    }
    return server.nextPendingConnection();
}

QString request_id(const QByteArray& request) {
    const auto document = QJsonDocument::fromJson(request);
    return document.isObject() ? document.object().value(QStringLiteral("request_id")).toString()
                               : QString{};
}

bool send_ready(QLocalSocket& socket, const QString& id,
                QJsonArray capabilities = {QStringLiteral("service.info")}) {
    const QJsonObject service{
        {QStringLiteral("api_version"), 1},
        {QStringLiteral("state"), 2},
        {QStringLiteral("service_version"), QStringLiteral("0.1.0")},
        {QStringLiteral("capabilities"), std::move(capabilities)},
    };
    const QJsonObject response{
        {QStringLiteral("schema_version"), 1},
        {QStringLiteral("request_id"), id},
        {QStringLiteral("kind"), 1},
        {QStringLiteral("boundary_error_code"), 0},
        {QStringLiteral("message_code"), QStringLiteral("service.ready")},
        {QStringLiteral("service"), service},
    };
    const auto encoded = frame(QJsonDocument(response).toJson(QJsonDocument::Compact));
    return socket.write(encoded) == encoded.size() && socket.waitForBytesWritten(1'000);
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
    const auto request = receive_frame(*socket, kShortTimeoutMilliseconds);
    const auto id = request_id(request);
    const QJsonArray unsorted{QStringLiteral("service.info"), QStringLiteral("repository.list")};
    bool passed =
        expect(send_ready(*socket, id, unsorted), "fake service sends invalid capability order") &&
        expect(
            wait_until([&] { return !client.errorText().isEmpty(); }, kShortTimeoutMilliseconds) &&
                !client.connected(),
            "desktop rejects capabilities that violate the contract");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_handshake_and_reconnect() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "fake service listener starts")) {
        return false;
    }
    ServiceClient client;
    auto* first_socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(first_socket != nullptr, "desktop connects to service endpoint")) {
        return false;
    }
    const auto first_request = receive_frame(*first_socket, kShortTimeoutMilliseconds);
    const auto first_id = request_id(first_request);
    bool passed =
        expect(!first_id.isEmpty(), "desktop sends a framed correlated request") &&
        expect(send_ready(*first_socket, first_id), "fake service sends Ready response") &&
        expect(wait_until([&] { return client.connected(); }, kShortTimeoutMilliseconds),
               "desktop enters Ready after a valid response") &&
        expect(client.serviceVersion() == QStringLiteral("0.1.0") && client.apiVersion() == 1 &&
                   client.capabilities() == QStringList{QStringLiteral("service.info")},
               "desktop publishes validated service information");
    first_socket->abort();
    passed &= expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds),
                     "desktop leaves Ready when service disconnects");

    auto* second_socket = accept_client(server, 3'500);
    if (!expect(second_socket != nullptr, "desktop automatically reconnects")) {
        return false;
    }
    const auto second_request = receive_frame(*second_socket, kShortTimeoutMilliseconds);
    const auto second_id = request_id(second_request);
    passed &= expect(!second_id.isEmpty() && second_id != first_id,
                     "reconnect creates a fresh request correlation id");
    passed &= expect(send_ready(*second_socket, QStringLiteral("wrong-request-id")),
                     "fake service sends mismatched response");
    passed &= expect(
        wait_until([&] { return !client.errorText().isEmpty(); }, kShortTimeoutMilliseconds) &&
            !client.connected(),
        "mismatched response never enters Ready");
    second_socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

} // namespace

int main(int argument_count, char* arguments[]) noexcept {
    QCoreApplication application(argument_count, arguments);
    try {
        return test_handshake_and_reconnect() && test_invalid_service_info() ? EXIT_SUCCESS
                                                                             : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
