#include "client/models/repository_connection_model.h"
#include "client/models/source_inventory_model.h"
#include "client/service_client.h"
#include "client/service_protocol.h"
#include "desktop_client_test_support.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSet>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <optional>

namespace {

using aegra::desktop::ServiceClient;
using namespace aegra::desktop;
using namespace aegra::desktop::test_support;

[[nodiscard]] QJsonObject source_item(const QString& source_id, const QString& name,
                                      const bool selectable, const qint64 availability = 1) {
    return {{QStringLiteral("source_id"), source_id},
            {QStringLiteral("display_name"), name},
            {QStringLiteral("kind"), 1},
            {QStringLiteral("availability"), availability},
            {QStringLiteral("capacity_bytes"), 1'073'741'824LL},
            {QStringLiteral("is_system"), false},
            {QStringLiteral("is_read_only"), !selectable && availability == 1},
            {QStringLiteral("is_selectable"), selectable}};
}

[[nodiscard]] QJsonObject connection_item(const QString& connection_id, const QString& name,
                                          const qint64 state, const bool is_default) {
    return {{QStringLiteral("connection_id"), connection_id},
            {QStringLiteral("display_name"), name},
            {QStringLiteral("state"), state},
            {QStringLiteral("is_default"), is_default},
            {QStringLiteral("capabilities"),
             QJsonArray{QStringLiteral("backup"), QStringLiteral("catalog")}}};
}

[[nodiscard]] QJsonObject command_ack(const QString& command_id, const qint64 disposition,
                                      const QString& resource_id) {
    return {{QStringLiteral("command_id"), command_id},
            {QStringLiteral("disposition"), disposition},
            {QStringLiteral("resource_id"), resource_id},
            {QStringLiteral("event_subscription"), QJsonValue(QJsonValue::Null)}};
}

bool send_page(QLocalSocket& socket, const QString& request_id, const qint64 request_kind,
               QJsonArray items, const std::optional<QString>& token) {
    const QJsonObject page{{QStringLiteral("items"), std::move(items)},
                           {QStringLiteral("continuation_token"),
                            token ? QJsonValue(*token) : QJsonValue(QJsonValue::Null)}};
    return send_object(socket, response(request_id, 1, request_kind, 0,
                                        QStringLiteral("control_plane.ready"), page));
}

bool test_codec_inventory_and_commands() {
    bool passed = true;

    SourceInventoryPage empty_page;
    const auto empty_body =
        QJsonDocument(
            response(
                QStringLiteral("req-empty"), 1, 4, 0, QStringLiteral("control_plane.ready"),
                QJsonObject{{QStringLiteral("items"), QJsonArray{}},
                            {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}}))
            .toJson(QJsonDocument::Compact);
    QJsonObject empty_root;
    passed &= expect(parse_response_root(empty_body, QStringLiteral("req-empty"), empty_root) &&
                         parse_source_inventory_response(empty_root, empty_page) &&
                         empty_page.items.isEmpty(),
                     "empty inventory page parses");

    const auto multi =
        QJsonDocument(
            response(
                QStringLiteral("req-multi"), 1, 4, 0, QStringLiteral("control_plane.ready"),
                QJsonObject{
                    {QStringLiteral("items"),
                     QJsonArray{
                         source_item(QStringLiteral("vol.aaa"), QStringLiteral("System"), true),
                         source_item(QStringLiteral("vol.bbb"), QStringLiteral("Data"), true),
                         source_item(QStringLiteral("vol.ccc"), QStringLiteral("Offline"), false,
                                     2)}},
                    {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}}))
            .toJson(QJsonDocument::Compact);
    QJsonObject multi_root;
    SourceInventoryPage multi_page;
    passed &= expect(parse_response_root(multi, QStringLiteral("req-multi"), multi_root) &&
                         parse_source_inventory_response(multi_root, multi_page) &&
                         multi_page.items.size() == 3,
                     "multi source inventory parses");
    auto rows = sources_from_variant_list(multi_page.items);
    SourceInventoryModel model;
    model.set_rows(rows);
    passed &= expect(model.selectableCount() == 2, "non-selectable sources counted out");

    const auto offline_conn =
        QJsonDocument(
            response(
                QStringLiteral("req-conn"), 1, 3, 0, QStringLiteral("control_plane.ready"),
                QJsonObject{{QStringLiteral("items"),
                             QJsonArray{connection_item(QStringLiteral("conn-a"),
                                                        QStringLiteral("Personal"), 2, true)}},
                            {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}}))
            .toJson(QJsonDocument::Compact);
    QJsonObject conn_root;
    RepositoryConnectionPage conn_page;
    passed &= expect(parse_response_root(offline_conn, QStringLiteral("req-conn"), conn_root) &&
                         parse_repository_connection_list_response(conn_root, conn_page) &&
                         conn_page.items.size() == 1,
                     "offline connection parses");
    RepositoryConnectionModel connections;
    connections.set_rows(connections_from_variant_list(conn_page.items));
    passed &= expect(connections.availableCount() == 0, "offline connection not available");

    const auto start_body =
        encode_start_backup_request(QStringLiteral("req-start"), QStringLiteral("idem-1"),
                                    QStringLiteral("vol.aaa"), QStringLiteral("conn-a"));
    const auto start_doc = QJsonDocument::fromJson(start_body);
    passed &= expect(start_doc.isObject() &&
                         start_doc.object().value(QStringLiteral("kind")).toInteger() == 37 &&
                         start_doc.object().value(QStringLiteral("idempotency_key")).toString() ==
                             QStringLiteral("idem-1") &&
                         !start_body.contains("password") && !start_body.contains("secret"),
                     "start backup encodes kind, key, no secrets");

    const auto add_body = encode_repository_connection_input_request(
        QStringLiteral("req-add"), QStringLiteral("idem-add"), kAddRepositoryConnectionRequestKind,
        QStringLiteral("Archive"), QStringLiteral("D:/Backup"));
    const auto add_object = QJsonDocument::fromJson(add_body).object();
    const auto add_payload = add_object.value(QStringLiteral("payload")).toObject();
    passed &= expect(add_object.value(QStringLiteral("kind")).toInteger() == 32 &&
                         add_payload.value(QStringLiteral("display_name")).toString() ==
                             QStringLiteral("Archive") &&
                         add_payload.value(QStringLiteral("locator")).toString() ==
                             QStringLiteral("D:/Backup") &&
                         add_payload.value(QStringLiteral("credential_ref")).isNull() &&
                         !add_body.contains("password"),
                     "repository input command encodes exact non-secret payload");

    const auto remove_body = encode_repository_connection_resource_request(
        QStringLiteral("req-remove"), QStringLiteral("idem-remove"),
        kRemoveRepositoryConnectionRequestKind, QStringLiteral("conn-a"));
    const auto remove_object = QJsonDocument::fromJson(remove_body).object();
    passed &= expect(remove_object.value(QStringLiteral("kind")).toInteger() == 36 &&
                         remove_object.value(QStringLiteral("payload"))
                                 .toObject()
                                 .value(QStringLiteral("resource_id"))
                                 .toString() == QStringLiteral("conn-a"),
                     "repository resource command encodes selected connection");

    const auto recovery_body = encode_recovery_point_request(
        QStringLiteral("req-recovery"), std::nullopt, QStringLiteral("conn-a"));
    const auto recovery_payload =
        QJsonDocument::fromJson(recovery_body).object().value(QStringLiteral("payload")).toObject();
    passed &=
        expect(recovery_payload.value(QStringLiteral("repository_connection_id")).toString() ==
                   QStringLiteral("conn-a"),
               "recovery query carries selected repository connection");

    const auto ack_body =
        QJsonDocument(response(QStringLiteral("req-start"), 2, 37, 0,
                               QStringLiteral("command.accepted"),
                               command_ack(QStringLiteral("cmd-1"), 1, QStringLiteral("job-abc"))))
            .toJson(QJsonDocument::Compact);
    QJsonObject ack_root;
    CommandAck ack;
    passed &= expect(parse_response_root(ack_body, QStringLiteral("req-start"), ack_root) &&
                         parse_command_ack_response(ack_root, 37, ack) &&
                         ack.resource_id == QStringLiteral("job-abc") && ack.disposition == 1,
                     "start backup accepted ack parses");

    const auto replay_body =
        QJsonDocument(response(QStringLiteral("req-start2"), 2, 37, 0,
                               QStringLiteral("command.replayed"),
                               command_ack(QStringLiteral("cmd-1"), 2, QStringLiteral("job-abc"))))
            .toJson(QJsonDocument::Compact);
    QJsonObject replay_root;
    CommandAck replay;
    passed &=
        expect(parse_response_root(replay_body, QStringLiteral("req-start2"), replay_root) &&
                   parse_command_ack_response(replay_root, 37, replay) && replay.disposition == 2,
               "start backup replayed ack parses");

    const auto fail_body =
        QJsonDocument(response(QStringLiteral("req-fail"), 3, 37, 9,
                               QStringLiteral("service.request_failed"), QJsonValue::Null))
            .toJson(QJsonDocument::Compact);
    QJsonObject fail_root;
    passed &= expect(parse_response_root(fail_body, QStringLiteral("req-fail"), fail_root) &&
                         is_command_failure_response(fail_root, 37),
                     "start backup failure rejected as failure response");

    const auto bad_secret =
        QJsonDocument(QJsonObject{{QStringLiteral("source_id"), QStringLiteral("vol.a")},
                                  {QStringLiteral("password"), QStringLiteral("hunter2")}})
            .toJson(QJsonDocument::Compact);
    passed &= expect(!QString::fromUtf8(bad_secret).isEmpty() &&
                         !QString::fromUtf8(start_body).contains(QStringLiteral("hunter2")),
                     "codec golden path never embeds plaintext secret fields");

    const auto cancel_body = encode_cancel_job_request(
        QStringLiteral("req-cancel"), QStringLiteral("cancel-key"), QStringLiteral("job-abc"));
    const auto cancel_doc = QJsonDocument::fromJson(cancel_body);
    passed &= expect(cancel_doc.isObject() &&
                         cancel_doc.object().value(QStringLiteral("kind")).toInteger() == 38,
                     "cancel job encodes kind 38");

    const auto cancel_ack_body =
        QJsonDocument(response(QStringLiteral("req-cancel"), 2, 38, 0,
                               QStringLiteral("command.accepted"),
                               command_ack(QStringLiteral("cmd-c"), 1, QStringLiteral("job-abc"))))
            .toJson(QJsonDocument::Compact);
    QJsonObject cancel_ack_root;
    CommandAck cancel_ack;
    passed &= expect(
        parse_response_root(cancel_ack_body, QStringLiteral("req-cancel"), cancel_ack_root) &&
            parse_command_ack_response(cancel_ack_root, 38, cancel_ack),
        "cancel accepted ack parses");

    // Malformed inventory: selectable while unavailable.
    const auto malformed =
        QJsonDocument(
            response(
                QStringLiteral("req-bad"), 1, 4, 0, QStringLiteral("control_plane.ready"),
                QJsonObject{{QStringLiteral("items"),
                             QJsonArray{QJsonObject{
                                 {QStringLiteral("source_id"), QStringLiteral("vol.bad")},
                                 {QStringLiteral("display_name"), QStringLiteral("Bad")},
                                 {QStringLiteral("kind"), 1},
                                 {QStringLiteral("availability"), 2},
                                 {QStringLiteral("capacity_bytes"), 1},
                                 {QStringLiteral("is_system"), false},
                                 {QStringLiteral("is_read_only"), false},
                                 {QStringLiteral("is_selectable"), true}}}},
                            {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}}))
            .toJson(QJsonDocument::Compact);
    QJsonObject bad_root;
    SourceInventoryPage bad_page;
    passed &= expect(parse_response_root(malformed, QStringLiteral("req-bad"), bad_root) &&
                         !parse_source_inventory_response(bad_root, bad_page),
                     "malformed selectable+unavailable source rejected");

    return passed;
}

bool drain_until_kinds(QLocalSocket& socket, QSet<qint64>& seen, const int timeout_ms) {
    QElapsedTimer timer;
    timer.start();
    while (timer.elapsed() < timeout_ms && seen.size() < 4) {
        const auto request = receive_object(socket, 400);
        if (request.isEmpty()) {
            continue;
        }
        seen.insert(request.value(QStringLiteral("kind")).toInteger());
        const auto kind = request.value(QStringLiteral("kind")).toInteger();
        const auto request_id = request.value(QStringLiteral("request_id")).toString();
        if (kind == 2) {
            const auto connection_id = request.value(QStringLiteral("payload"))
                                           .toObject()
                                           .value(QStringLiteral("repository_connection_id"))
                                           .toString();
            send_not_configured(socket, request_id,
                                connection_id.isEmpty() ? std::nullopt
                                                        : std::optional{connection_id});
        } else if (kind == 3) {
            send_page(
                socket, request_id, 3,
                {connection_item(QStringLiteral("conn-1"), QStringLiteral("Personal"), 1, true)},
                std::nullopt);
        } else if (kind == 4) {
            send_page(socket, request_id, 4,
                      {source_item(QStringLiteral("vol.aaa"), QStringLiteral("System"), true),
                       source_item(QStringLiteral("vol.bbb"), QStringLiteral("Data"), true)},
                      std::nullopt);
        } else if (kind == 5) {
            send_object(socket, response(request_id, 1, 5, 0, QStringLiteral("job.list_ready"),
                                         QJsonObject{{QStringLiteral("items"), QJsonArray{}},
                                                     {QStringLiteral("continuation_token"),
                                                      QJsonValue(QJsonValue::Null)}}));
        }
    }
    return seen.contains(2) && seen.contains(3) && seen.contains(4);
}

bool test_service_client_start_cancel_and_observe() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "backup client listener starts")) {
        return false;
    }
    ServiceClient client;
    auto* socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(socket != nullptr, "backup client connects")) {
        return false;
    }

    const auto info_request = receive_object(*socket, kShortTimeoutMilliseconds);
    const QJsonArray capabilities{
        QStringLiteral("backup.start"),    QStringLiteral("job.cancel"),
        QStringLiteral("job.list"),        QStringLiteral("repository.connection"),
        QStringLiteral("repository.list"), QStringLiteral("service.info"),
        QStringLiteral("source.inventory")};
    bool passed = expect(
        valid_request(info_request, 1) &&
            send_service_info(*socket, info_request.value(QStringLiteral("request_id")).toString(),
                              capabilities),
        "service info with backup capabilities accepted");
    passed &= expect(wait_until(
                         [&] {
                             return client.connected() && client.backupStartAvailable() &&
                                    client.inventoryAvailable() && client.connectionsAvailable();
                         },
                         kShortTimeoutMilliseconds),
                     "client ready with backup capabilities");

    QSet<qint64> seen;
    passed &= expect(drain_until_kinds(*socket, seen, kShortTimeoutMilliseconds),
                     "inventory and connection queries issued with repository/jobs");
    passed &= expect(wait_until(
                         [&] {
                             return client.sources()->rowCount() == 2 &&
                                    client.connections()->rowCount() == 1 &&
                                    client.connections()->availableCount() == 1;
                         },
                         kShortTimeoutMilliseconds),
                     "inventory and connection models publish");

    client.testRepositoryConnection(QStringLiteral("conn-1"));
    const auto test_request = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(test_request.value(QStringLiteral("kind")).toInteger() == 34 &&
                         test_request.value(QStringLiteral("payload"))
                                 .toObject()
                                 .value(QStringLiteral("resource_id"))
                                 .toString() == QStringLiteral("conn-1") &&
                         client.repositoryCommandBusy(),
                     "test connection sends selected resource command");
    passed &= expect(
        send_object(*socket,
                    response(test_request.value(QStringLiteral("request_id")).toString(), 2, 34, 0,
                             QStringLiteral("command.accepted"),
                             command_ack(QStringLiteral("cmd-test"), 1, QStringLiteral("conn-1")))),
        "test connection acknowledgement sent");
    const auto refreshed_connections = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(
        refreshed_connections.value(QStringLiteral("kind")).toInteger() == 3 &&
            send_page(
                *socket, refreshed_connections.value(QStringLiteral("request_id")).toString(), 3,
                {connection_item(QStringLiteral("conn-1"), QStringLiteral("Personal"), 1, true)},
                std::nullopt),
        "successful command refreshes connection list");
    const auto refreshed_catalog = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(
        refreshed_catalog.value(QStringLiteral("kind")).toInteger() == 2 &&
            send_not_configured(*socket,
                                refreshed_catalog.value(QStringLiteral("request_id")).toString(),
                                QStringLiteral("conn-1")) &&
            wait_until([&] { return !client.repositoryCommandBusy(); }, kShortTimeoutMilliseconds),
        "connection refresh reloads selected catalog");

    client.startBackup(QStringLiteral("vol.aaa"), QStringLiteral("conn-1"));
    const auto start_request = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &=
        expect(start_request.value(QStringLiteral("schema_version")).toInteger() == 3 &&
                   start_request.value(QStringLiteral("kind")).toInteger() == 37 &&
                   start_request.value(QStringLiteral("idempotency_key")).isString() &&
                   !start_request.value(QStringLiteral("idempotency_key")).toString().isEmpty() &&
                   start_request.value(QStringLiteral("payload")).isObject(),
               "start backup sends kind 37 with idempotency key");
    const auto start_payload = start_request.value(QStringLiteral("payload")).toObject();
    passed &= expect(start_payload.value(QStringLiteral("source_id")).toString() ==
                             QStringLiteral("vol.aaa") &&
                         start_payload.value(QStringLiteral("backup_type")).toInteger() == 1 &&
                         start_payload.value(QStringLiteral("parent_recovery_point_id")).isNull(),
                     "start backup payload is full-only without parent");

    const auto idem_key = start_request.value(QStringLiteral("idempotency_key")).toString();
    passed &= expect(
        send_object(*socket, response(start_request.value(QStringLiteral("request_id")).toString(),
                                      2, 37, 0, QStringLiteral("command.accepted"),
                                      command_ack(QStringLiteral("cmd-1"), 1,
                                                  QStringLiteral("job-backup-1")))),
        "start backup accepted");
    passed &= expect(
        wait_until([&] { return client.activeBackupJobId() == QStringLiteral("job-backup-1"); },
                   kShortTimeoutMilliseconds),
        "active backup job id published from ack");

    // Duplicate click while active must not send another Start (kind 37).
    client.startBackup(QStringLiteral("vol.aaa"), QStringLiteral("conn-1"));

    // After accept, client refreshes job.list; answer with a running backup job.
    QElapsedTimer poll_wait;
    poll_wait.start();
    bool answered_job = false;
    bool saw_second_start = false;
    QJsonObject cancel_request;
    while (poll_wait.elapsed() < 6'000 && (!answered_job || cancel_request.isEmpty())) {
        const auto request = receive_object(*socket, 400);
        if (request.isEmpty()) {
            if (answered_job && cancel_request.isEmpty() && client.activeBackupCancellable()) {
                client.cancelActiveBackup();
            }
            continue;
        }
        const auto kind = request.value(QStringLiteral("kind")).toInteger();
        const auto request_id = request.value(QStringLiteral("request_id")).toString();
        if (kind == 37) {
            saw_second_start = true;
        } else if (kind == 5) {
            const QJsonObject job{
                {QStringLiteral("job_id"), QStringLiteral("job-backup-1")},
                {QStringLiteral("trace_id"), QStringLiteral("trace-1")},
                {QStringLiteral("operation"), 1},
                {QStringLiteral("state"), 2},
                {QStringLiteral("created_utc_ms"), 1'700'000'000'000LL},
                {QStringLiteral("started_utc_ms"), 1'700'000'000'100LL},
                {QStringLiteral("completed_utc_ms"), QJsonValue(QJsonValue::Null)},
                {QStringLiteral("progress"),
                 QJsonObject{{QStringLiteral("schema_version"), 1},
                             {QStringLiteral("job_id"), QStringLiteral("job-backup-1")},
                             {QStringLiteral("trace_id"), QStringLiteral("trace-1")},
                             {QStringLiteral("phase"), 2},
                             {QStringLiteral("logical_bytes"), 100},
                             {QStringLiteral("processed_bytes"), 40},
                             {QStringLiteral("stored_bytes"), 20},
                             {QStringLiteral("message_code"), QStringLiteral("job.progress")}}},
                {QStringLiteral("message_code"), QStringLiteral("job.running")}};
            answered_job = send_object(
                *socket, response(request_id, 1, 5, 0, QStringLiteral("job.list_ready"),
                                  QJsonObject{{QStringLiteral("items"), QJsonArray{job}},
                                              {QStringLiteral("continuation_token"),
                                               QJsonValue(QJsonValue::Null)}}));
        } else if (kind == 38) {
            cancel_request = request;
        } else if (kind == 2) {
            send_not_configured(*socket, request_id);
        } else if (kind == 3 || kind == 4) {
            send_page(*socket, request_id, static_cast<int>(kind), {}, std::nullopt);
        }
    }
    passed &= expect(!saw_second_start, "duplicate start while active does not send");
    passed &= expect(answered_job, "job list poll answered with running backup");
    passed &= expect(wait_until(
                         [&] {
                             return client.activeBackupCancellable() &&
                                    client.activeBackupProgressPercent() == 40;
                         },
                         kShortTimeoutMilliseconds),
                     "observed job progress and cancellable state");

    if (cancel_request.isEmpty()) {
        client.cancelActiveBackup();
        cancel_request = receive_object(*socket, kShortTimeoutMilliseconds);
        // Drain non-cancel frames if job poll arrives first.
        QElapsedTimer cancel_wait;
        cancel_wait.start();
        while (cancel_wait.elapsed() < kShortTimeoutMilliseconds &&
               cancel_request.value(QStringLiteral("kind")).toInteger() != 38) {
            const auto kind = cancel_request.value(QStringLiteral("kind")).toInteger();
            const auto request_id = cancel_request.value(QStringLiteral("request_id")).toString();
            if (kind == 5) {
                send_object(*socket, response(request_id, 1, 5, 0, QStringLiteral("job.list_ready"),
                                              QJsonObject{{QStringLiteral("items"), QJsonArray{}},
                                                          {QStringLiteral("continuation_token"),
                                                           QJsonValue(QJsonValue::Null)}}));
            }
            cancel_request = receive_object(*socket, 400);
        }
    }
    passed &= expect(cancel_request.value(QStringLiteral("kind")).toInteger() == 38 &&
                         cancel_request.value(QStringLiteral("idempotency_key")).isString() &&
                         cancel_request.value(QStringLiteral("payload"))
                                 .toObject()
                                 .value(QStringLiteral("resource_id"))
                                 .toString() == QStringLiteral("job-backup-1"),
                     "cancel sends kind 38 for active job");
    passed &= expect(
        send_object(*socket, response(cancel_request.value(QStringLiteral("request_id")).toString(),
                                      2, 38, 0, QStringLiteral("command.accepted"),
                                      command_ack(QStringLiteral("cmd-cancel"), 1,
                                                  QStringLiteral("job-backup-1")))),
        "cancel accepted");
    passed &=
        expect(wait_until([&] { return !client.cancelCommandBusy(); }, kShortTimeoutMilliseconds),
               "cancel command completes");

    // Idempotency key on start was unique and non-empty.
    passed &= expect(!idem_key.isEmpty(), "start used non-empty idempotency key");

    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

} // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    bool passed = true;
    passed &= test_codec_inventory_and_commands();
    passed &= test_service_client_start_cancel_and_observe();
    if (!passed) {
        std::fprintf(stderr, "desktop_backup_client_test failed\n");
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "desktop_backup_client_test passed\n");
    return EXIT_SUCCESS;
}
