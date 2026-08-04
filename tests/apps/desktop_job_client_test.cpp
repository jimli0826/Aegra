#include "client/models/job_model.h"
#include "client/service_client.h"
#include "client/service_protocol.h"
#include "desktop_client_test_support.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QVariantMap>

#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <utility>

namespace {

using aegra::desktop::ServiceClient;
using namespace aegra::desktop::test_support;

bool send_job_page(QLocalSocket& socket, const QString& request_id, QJsonArray items,
                   const std::optional<QString>& token) {
    const QJsonObject page{{QStringLiteral("items"), std::move(items)},
                           {QStringLiteral("continuation_token"),
                            token ? QJsonValue(*token) : QJsonValue(QJsonValue::Null)}};
    return send_object(socket,
                       response(request_id, 1, 5, 0, QStringLiteral("job.list_ready"), page));
}

QJsonObject job_summary(const QString& job_id, const qint64 state) {
    const auto message_code = state == 4   ? QStringLiteral("job.succeeded")
                              : state == 5 ? QStringLiteral("job.failed")
                                           : QStringLiteral("job.running");
    return {{QStringLiteral("job_id"), job_id},
            {QStringLiteral("trace_id"), QStringLiteral("trace-") + job_id},
            {QStringLiteral("operation"), 1},
            {QStringLiteral("state"), state},
            {QStringLiteral("created_utc_ms"), 1'700'000'000'000LL},
            {QStringLiteral("started_utc_ms"), 1'700'000'000'100LL},
            {QStringLiteral("completed_utc_ms"),
             state >= 4 ? QJsonValue(1'700'000'000'200LL) : QJsonValue(QJsonValue::Null)},
            {QStringLiteral("progress"), QJsonValue(QJsonValue::Null)},
            {QStringLiteral("message_code"), message_code}};
}

bool collect_initial_queries(QLocalSocket& socket, QJsonObject& repository_request,
                             QJsonObject& job_request) {
    QElapsedTimer collect;
    collect.start();
    while (collect.elapsed() < kShortTimeoutMilliseconds &&
           (repository_request.isEmpty() || job_request.isEmpty())) {
        const auto request = receive_object(socket, 500);
        const auto kind = request.value(QStringLiteral("kind")).toInteger();
        if (kind == 2 && repository_request.isEmpty()) {
            repository_request = request;
        } else if (kind == 5 && job_request.isEmpty()) {
            job_request = request;
        }
    }
    return valid_request(repository_request, 2) && valid_request(job_request, 5);
}

bool send_concurrent_query_responses(QLocalSocket& socket, const QJsonObject& repository_request,
                                     const QJsonObject& job_request) {
    if (!send_job_page(
            socket, job_request.value(QStringLiteral("request_id")).toString(),
            {job_summary(QStringLiteral("job-1"), 2), job_summary(QStringLiteral("job-2"), 4)},
            std::optional<QString>{QStringLiteral("job-token-1")})) {
        return false;
    }
    const auto continuation = receive_object(socket, kShortTimeoutMilliseconds);
    return valid_request(continuation, 5) &&
           send_job_page(socket, continuation.value(QStringLiteral("request_id")).toString(),
                         {job_summary(QStringLiteral("job-3"), 5)}, std::nullopt) &&
           send_catalog_page(
               socket, repository_request.value(QStringLiteral("request_id")).toString(),
               {recovery_point(QLatin1String(kFirstFileUuid), std::nullopt, 1)}, std::nullopt);
}

bool test_job_list_and_concurrent_repository() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!expect(server.listen(QLatin1String(kPipeName)), "job list listener starts")) {
        return false;
    }
    ServiceClient client;
    auto* socket = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(socket != nullptr, "job list client connects")) {
        return false;
    }
    const auto info_request = receive_object(*socket, kShortTimeoutMilliseconds);
    const QJsonArray capabilities{QStringLiteral("job.list"), QStringLiteral("repository.list"),
                                  QStringLiteral("service.info")};
    bool passed = expect(
        valid_request(info_request, 1) &&
            send_service_info(*socket, info_request.value(QStringLiteral("request_id")).toString(),
                              capabilities),
        "service info with job.list accepted");
    passed &= expect(wait_until([&] { return client.connected() && client.jobListAvailable(); },
                                kShortTimeoutMilliseconds),
                     "client ready with job.list capability");

    QJsonObject repository_request;
    QJsonObject job_request;
    passed &= expect(collect_initial_queries(*socket, repository_request, job_request),
                     "repository and job queries are both issued");
    if (!passed) {
        socket->abort();
        server.close();
        QLocalServer::removeServer(QLatin1String(kPipeName));
        return passed;
    }

    passed &= expect(send_concurrent_query_responses(*socket, repository_request, job_request),
                     "concurrent job and repository pages answered");

    passed &= expect(wait_until(
                         [&] {
                             return client.connected() && client.recoveryPointCount() == 1 &&
                                    client.jobs()->rowCount() == 3 &&
                                    client.jobs()->runningCount() == 1 &&
                                    client.jobs()->failedCount() == 1 &&
                                    client.jobs()->succeededCount() == 1;
                         },
                         kShortTimeoutMilliseconds),
                     "repository and job models publish concurrently without loss");

    // Refresh repository while jobs are idle should still work.
    client.refreshRepository();
    const auto refresh = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(
        valid_request(refresh, 2) &&
            send_not_configured(*socket, refresh.value(QStringLiteral("request_id")).toString()),
        "repository refresh remains independent of jobs");
    passed &= expect(
        wait_until([&] { return !client.repositoryConfigured(); }, kShortTimeoutMilliseconds),
        "repository refresh result applied");

    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool handshake_job_list(QLocalServer& server, ServiceClient& client, QLocalSocket*& socket,
                        QJsonObject& job_request) {
    socket = accept_client(server, kShortTimeoutMilliseconds);
    if (socket == nullptr) {
        return false;
    }
    const auto info_request = receive_object(*socket, kShortTimeoutMilliseconds);
    const QJsonArray capabilities{QStringLiteral("job.list"), QStringLiteral("repository.list"),
                                  QStringLiteral("service.info")};
    if (!valid_request(info_request, 1) ||
        !send_service_info(*socket, info_request.value(QStringLiteral("request_id")).toString(),
                           capabilities)) {
        return false;
    }
    if (!wait_until([&] { return client.connected() && client.jobListAvailable(); },
                    kShortTimeoutMilliseconds)) {
        return false;
    }
    QJsonObject repository_request;
    QElapsedTimer collect;
    collect.start();
    while (collect.elapsed() < kShortTimeoutMilliseconds &&
           (repository_request.isEmpty() || job_request.isEmpty())) {
        const auto request = receive_object(*socket, 500);
        if (request.isEmpty()) {
            continue;
        }
        const auto kind = request.value(QStringLiteral("kind")).toInteger();
        if (kind == 2) {
            repository_request = request;
        } else if (kind == 5) {
            job_request = request;
        }
    }
    if (!valid_request(repository_request, 2) || !valid_request(job_request, 5)) {
        return false;
    }
    // Satisfy repository with not-configured so only jobs matter for remaining assertions.
    return send_not_configured(*socket,
                               repository_request.value(QStringLiteral("request_id")).toString());
}

bool test_job_empty_page_and_baseline_no_toast() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    bool passed =
        expect(send_job_page(*socket, job_request.value(QStringLiteral("request_id")).toString(),
                             {job_summary(QStringLiteral("job-hist"), 4)}, std::nullopt),
               "history terminal job page");
    passed &= expect(
        wait_until([&] { return client.jobs()->rowCount() == 1; }, kShortTimeoutMilliseconds),
        "history job published");
    passed &=
        expect(!client.toastVisible(), "first snapshot does not toast historical terminal jobs");

    // Second poll: same terminal job must not re-toast; new terminal job should toast.
    client.refreshJobs();
    const auto second = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &=
        expect(valid_request(second, 5) &&
                   send_job_page(*socket, second.value(QStringLiteral("request_id")).toString(),
                                 {job_summary(QStringLiteral("job-hist"), 4),
                                  job_summary(QStringLiteral("job-new"), 5)},
                                 std::nullopt),
               "second job snapshot with new failure");
    passed &=
        expect(wait_until([&] { return client.jobs()->rowCount() == 2 && client.toastVisible(); },
                          kShortTimeoutMilliseconds),
               "only newly terminal job toasts");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

[[nodiscard]] QJsonObject job_list_root(QJsonArray items, QJsonValue continuation) {
    return {{QStringLiteral("schema_version"), 3},
            {QStringLiteral("message_type"), 2},
            {QStringLiteral("request_id"), QStringLiteral("r1")},
            {QStringLiteral("kind"), 1},
            {QStringLiteral("request_kind"), 5},
            {QStringLiteral("boundary_error_code"), 0},
            {QStringLiteral("message_code"), QStringLiteral("job.list_ready")},
            {QStringLiteral("message_arguments"), QJsonArray{}},
            {QStringLiteral("payload"),
             QJsonObject{{QStringLiteral("items"), std::move(items)},
                         {QStringLiteral("continuation_token"), std::move(continuation)}}}};
}

[[nodiscard]] QJsonObject progress_object(const QString& job_id, const QString& trace_id,
                                          const qint64 schema_version, const qint64 logical,
                                          const qint64 processed, const qint64 stored,
                                          const QString& message_code) {
    return {{QStringLiteral("schema_version"), schema_version},
            {QStringLiteral("job_id"), job_id},
            {QStringLiteral("trace_id"), trace_id},
            {QStringLiteral("phase"), 2},
            {QStringLiteral("logical_bytes"), logical},
            {QStringLiteral("processed_bytes"), processed},
            {QStringLiteral("stored_bytes"), stored},
            {QStringLiteral("message_code"), message_code}};
}

bool test_job_page_protocol() {
    using aegra::desktop::JobPage;
    using aegra::desktop::parse_job_list_response;

    bool passed = true;
    JobPage empty_page;
    passed &= expect(
        parse_job_list_response(job_list_root({}, QJsonValue(QJsonValue::Null)), empty_page) &&
            empty_page.items.isEmpty(),
        "empty job page accepted");
    QJsonArray items;
    for (int index = 0; index < 101; ++index) {
        items.append(job_summary(QStringLiteral("job-%1").arg(index), 4));
    }
    JobPage oversized_page;
    passed &=
        expect(!parse_job_list_response(
                   job_list_root(std::move(items), QJsonValue(QJsonValue::Null)), oversized_page),
               "page with more than 100 jobs rejected");
    return passed;
}

bool progress_is_rejected(const QJsonObject& progress) {
    auto job = job_summary(QStringLiteral("job-1"), 2);
    job.insert(QStringLiteral("progress"), progress);
    QVariantMap parsed;
    return !aegra::desktop::parse_job_summary_object(job, parsed);
}

bool test_job_progress_protocol() {
    bool passed = true;
    passed &= expect(progress_is_rejected(progress_object(QStringLiteral("job-other"),
                                                          QStringLiteral("trace-job-1"), 1, 100, 10,
                                                          5, QStringLiteral("job.running"))),
                     "progress job_id mismatch rejected");
    passed &= expect(
        progress_is_rejected(progress_object(QStringLiteral("job-1"), QStringLiteral("trace-other"),
                                             1, 100, 10, 5, QStringLiteral("job.running"))),
        "progress trace_id mismatch rejected");
    passed &= expect(
        progress_is_rejected(progress_object(QStringLiteral("job-1"), QStringLiteral("trace-job-1"),
                                             2, 100, 10, 5, QStringLiteral("job.running"))),
        "progress schema_version mismatch rejected");
    passed &= expect(
        progress_is_rejected(progress_object(QStringLiteral("job-1"), QStringLiteral("trace-job-1"),
                                             1, 100, 10, 5, QString{})),
        "progress empty message_code rejected");
    passed &= expect(
        progress_is_rejected(progress_object(QStringLiteral("job-1"), QStringLiteral("trace-job-1"),
                                             1, 10, 11, 1, QStringLiteral("job.running"))),
        "processed_bytes > logical_bytes rejected");
    passed &= expect(
        progress_is_rejected(progress_object(QStringLiteral("job-1"), QStringLiteral("trace-job-1"),
                                             1, 100, 10, -1, QStringLiteral("job.running"))),
        "negative stored_bytes rejected");
    return passed;
}

bool test_job_progress_model() {
    auto job = job_summary(QStringLiteral("job-1"), 2);
    job.insert(QStringLiteral("progress"),
               progress_object(QStringLiteral("job-1"), QStringLiteral("trace-job-1"), 1, 200, 50,
                               40, QStringLiteral("job.running")));
    QVariantMap parsed;
    bool passed =
        expect(aegra::desktop::parse_job_summary_object(job, parsed), "valid progress accepted");
    aegra::desktop::JobModel model;
    model.set_rows(aegra::desktop::jobs_from_variant_list({parsed}));
    passed &=
        expect(model.data(model.index(0, 0), aegra::desktop::JobModel::ProgressPercentRole) == 25,
               "progress percent is 25");

    QVariantMap large{{QStringLiteral("jobId"), QStringLiteral("job-big")},
                      {QStringLiteral("traceId"), QStringLiteral("trace-big")},
                      {QStringLiteral("operation"), 1},
                      {QStringLiteral("state"), 2},
                      {QStringLiteral("createdUtcMs"), 1},
                      {QStringLiteral("messageCode"), QStringLiteral("job.running")},
                      {QStringLiteral("progressLogicalBytes"),
                       static_cast<qint64>((std::numeric_limits<qint64>::max)() / 2)},
                      {QStringLiteral("progressProcessedBytes"),
                       static_cast<qint64>((std::numeric_limits<qint64>::max)() / 4)}};
    model.set_rows(aegra::desktop::jobs_from_variant_list({large}));
    passed &=
        expect(model.data(model.index(0, 0), aegra::desktop::JobModel::ProgressPercentRole) == 50,
               "large progress percent stays 50 without overflow");
    large.insert(QStringLiteral("progressProcessedBytes"),
                 static_cast<qint64>((std::numeric_limits<qint64>::max)() / 2) + 1);
    model.set_rows(aegra::desktop::jobs_from_variant_list({large}));
    passed &=
        expect(model.data(model.index(0, 0), aegra::desktop::JobModel::ProgressPercentRole) == 0,
               "processed > logical yields 0 percent");
    return passed;
}

bool test_job_duplicate_token_and_cap() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    // Token does not advance: response token equals the token the client just requested.
    bool passed =
        expect(send_job_page(*socket, job_request.value(QStringLiteral("request_id")).toString(),
                             {job_summary(QStringLiteral("job-a"), 2)},
                             std::optional<QString>{QStringLiteral("same-token")}),
               "first page with token");
    const auto cont = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(valid_request(cont, 5) &&
                         send_job_page(*socket, cont.value(QStringLiteral("request_id")).toString(),
                                       {job_summary(QStringLiteral("job-b"), 2)},
                                       std::optional<QString>{QStringLiteral("same-token")}),
                     "repeated token page");
    passed &= expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds),
                     "non-advancing continuation token disconnects client");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_job_cumulative_cap() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    // Fill exactly 10,000 jobs across 100 pages, then one more job must trip the cumulative cap.
    QString request_id = job_request.value(QStringLiteral("request_id")).toString();
    constexpr int kPageSize = 100;
    constexpr int kFullPages = 100;
    for (int page = 0; page < kFullPages; ++page) {
        QJsonArray items;
        for (int index = 0; index < kPageSize; ++index) {
            items.append(job_summary(QStringLiteral("job-%1").arg(page * kPageSize + index), 4));
        }
        const auto page_token = std::optional<QString>{QStringLiteral("tok-%1").arg(page + 1)};
        if (!send_job_page(*socket, request_id, std::move(items), page_token)) {
            return expect(false, "send full job page for cumulative cap");
        }
        const auto next = receive_object(*socket, kShortTimeoutMilliseconds);
        if (!valid_request(next, 5)) {
            return expect(false, "continuation request for cumulative pages");
        }
        request_id = next.value(QStringLiteral("request_id")).toString();
    }
    bool passed =
        expect(send_job_page(*socket, request_id, {job_summary(QStringLiteral("job-overflow"), 4)},
                             std::nullopt),
               "overflow page sent");
    passed &= expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds),
                     "cumulative job cap disconnects client");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_job_duplicate_id_rejection() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    bool passed =
        expect(send_job_page(*socket, job_request.value(QStringLiteral("request_id")).toString(),
                             {job_summary(QStringLiteral("job-dup"), 2)},
                             std::optional<QString>{QStringLiteral("t1")}),
               "first page");
    const auto cont = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(valid_request(cont, 5) &&
                         send_job_page(*socket, cont.value(QStringLiteral("request_id")).toString(),
                                       {job_summary(QStringLiteral("job-dup"), 2)}, std::nullopt),
                     "duplicate job id on next page");
    passed &= expect(wait_until([&] { return !client.connected(); }, kShortTimeoutMilliseconds),
                     "duplicate job id disconnects");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_job_disconnect_clears_and_requeries() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    bool passed =
        expect(send_job_page(*socket, job_request.value(QStringLiteral("request_id")).toString(),
                             {job_summary(QStringLiteral("job-live"), 2)}, std::nullopt),
               "initial job page");
    passed &= expect(
        wait_until([&] { return client.jobs()->rowCount() == 1; }, kShortTimeoutMilliseconds),
        "job loaded");
    socket->abort();
    passed &=
        expect(wait_until([&] { return !client.connected() && client.jobs()->rowCount() == 0; },
                          kShortTimeoutMilliseconds),
               "disconnect clears jobs");

    // Reconnect path: accept new client connection after auto-reconnect.
    auto* socket2 = accept_client(server, kShortTimeoutMilliseconds);
    if (!expect(socket2 != nullptr, "client reconnects")) {
        return false;
    }
    QJsonObject job2;
    // Drain handshake + queries again.
    const auto info2 = receive_object(*socket2, kShortTimeoutMilliseconds);
    const QJsonArray capabilities{QStringLiteral("job.list"), QStringLiteral("repository.list"),
                                  QStringLiteral("service.info")};
    passed &=
        expect(send_service_info(*socket2, info2.value(QStringLiteral("request_id")).toString(),
                                 capabilities),
               "reconnect service info");
    QJsonObject repo2;
    QElapsedTimer collect;
    collect.start();
    while (collect.elapsed() < kShortTimeoutMilliseconds && (repo2.isEmpty() || job2.isEmpty())) {
        const auto request = receive_object(*socket2, 500);
        if (request.isEmpty()) {
            continue;
        }
        if (request.value(QStringLiteral("kind")).toInteger() == 2) {
            repo2 = request;
        } else if (request.value(QStringLiteral("kind")).toInteger() == 5) {
            job2 = request;
        }
    }
    passed &= expect(valid_request(repo2, 2) && valid_request(job2, 5), "reconnect re-queries");
    passed &= expect(
        send_not_configured(*socket2, repo2.value(QStringLiteral("request_id")).toString()) &&
            send_job_page(*socket2, job2.value(QStringLiteral("request_id")).toString(),
                          {job_summary(QStringLiteral("job-live"), 2)}, std::nullopt),
        "reconnect responses");
    passed &=
        expect(wait_until([&] { return client.connected() && client.jobs()->rowCount() == 1; },
                          kShortTimeoutMilliseconds),
               "jobs restored after reconnect requery");
    socket2->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_poll_non_overlapping() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    // Keep a running job so polling is armed.
    bool passed =
        expect(send_job_page(*socket, job_request.value(QStringLiteral("request_id")).toString(),
                             {job_summary(QStringLiteral("job-run"), 2)}, std::nullopt),
               "running job seed");
    passed &= expect(
        wait_until([&] { return client.jobs()->activeCount() == 1; }, kShortTimeoutMilliseconds),
        "active job enables polling");
    // Start a manual refresh and ensure a second refresh does not open another request while
    // pending.
    client.refreshJobs();
    const auto poll1 = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(valid_request(poll1, 5), "poll/refresh request received");
    passed &= expect(client.jobsLoading() && !client.globalLoading(),
                     "background job refresh does not block the full desktop");
    client.refreshJobs();
    // No second request should arrive quickly while first is outstanding.
    const auto maybe_second = receive_object(*socket, 300);
    passed &= expect(maybe_second.isEmpty(), "overlapping refresh is suppressed");
    passed &= expect(send_job_page(*socket, poll1.value(QStringLiteral("request_id")).toString(),
                                   {job_summary(QStringLiteral("job-run"), 2)}, std::nullopt),
                     "complete outstanding poll");
    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

bool test_job_timeout_and_error_payload() {
    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    if (!server.listen(QLatin1String(kPipeName))) {
        return false;
    }
    ServiceClient client;
    QLocalSocket* socket = nullptr;
    QJsonObject job_request;
    if (!handshake_job_list(server, client, socket, job_request)) {
        return false;
    }
    // Explicit failure payload keeps the Ready session and surfaces job error text.
    bool passed = expect(
        send_object(*socket,
                    response(job_request.value(QStringLiteral("request_id")).toString(), 3, 5, 5,
                             QStringLiteral("job.query_failed"), QJsonValue(QJsonValue::Null))),
        "job error payload");
    passed &= expect(
        wait_until([&] { return !client.jobsErrorText().isEmpty(); }, kShortTimeoutMilliseconds),
        "job error text published without crashing session");
    passed &= expect(client.connected(), "job query failure keeps service connection");

    // Request timeout: leave a subsequent job.list unanswered until coordinator deadline (30s).
    client.refreshJobs();
    const auto timed = receive_object(*socket, kShortTimeoutMilliseconds);
    passed &= expect(valid_request(timed, 5), "timeout subject job request received");
    // Do not answer; wait for service.request_timeout → disconnect + reconnect attempt.
    constexpr int kDeadlineWaitMilliseconds = 35'000;
    passed &= expect(wait_until([&] { return !client.connected(); }, kDeadlineWaitMilliseconds),
                     "unanswered job request times out and disconnects");

    socket->abort();
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));
    return passed;
}

} // namespace

int main(int argument_count, char* arguments[]) noexcept {
    QCoreApplication application(argument_count, arguments);
    try {
        const bool passed = test_job_list_and_concurrent_repository() &&
                            test_job_empty_page_and_baseline_no_toast() &&
                            test_job_page_protocol() && test_job_progress_protocol() &&
                            test_job_progress_model() && test_job_duplicate_token_and_cap() &&
                            test_job_cumulative_cap() && test_job_duplicate_id_rejection() &&
                            test_job_disconnect_clears_and_requeries() &&
                            test_poll_non_overlapping() && test_job_timeout_and_error_payload();
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
