#include "client/service_client.h"
#include "locale/locale_controller.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QHash>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QTimer>
#include <QUrl>

#include <cstdio>
#include <cstdlib>
#include <functional>
#include <utility>
#include <vector>

namespace {

constexpr auto kPipeName = "aegra-service-control";

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

[[nodiscard]] QString evidence_root() {
    const auto from_env = qEnvironmentVariable("AEGRA_D2_EVIDENCE_DIR");
    if (!from_env.isEmpty()) {
        return from_env;
    }
    return QDir(QStringLiteral(AEGRA_D2_EVIDENCE_DIR)).absolutePath();
}

void process_for(const int milliseconds) {
    QEventLoop loop;
    QTimer::singleShot(milliseconds, &loop, &QEventLoop::quit);
    loop.exec();
}

[[nodiscard]] bool capture_window(QQuickWindow* window, const QString& path) {
    if (window == nullptr) {
        return false;
    }
    window->requestUpdate();
    process_for(80);
    const auto image = window->grabWindow();
    if (image.isNull() || image.width() < 100 || image.height() < 100) {
        std::fprintf(stderr, "[FAIL] grabWindow empty for %s\n", path.toUtf8().constData());
        return false;
    }
    if (!QDir().mkpath(QFileInfo(path).absolutePath())) {
        std::fprintf(stderr, "[FAIL] mkpath for %s\n", path.toUtf8().constData());
        return false;
    }
    if (QFile::exists(path) && !QFile::remove(path)) {
        std::fprintf(stderr, "[FAIL] remove old PNG %s\n", path.toUtf8().constData());
        return false;
    }
    const bool save_succeeded = image.save(path, "PNG");
    QFile output(path);
    if (!output.open(QIODevice::ReadOnly)) {
        std::fprintf(stderr, "[FAIL] reopen PNG %s\n", path.toUtf8().constData());
        return false;
    }
    const auto signature = output.read(8);
    const QByteArray png_signature{"\x89PNG\r\n\x1a\n", 8};
    if (signature != png_signature || output.size() <= 8) {
        std::fprintf(stderr, "[FAIL] invalid PNG %s\n", path.toUtf8().constData());
        return false;
    }
    output.close();
    const QImage saved_image(path, "PNG");
    if (saved_image.isNull() || saved_image.size() != image.size()) {
        std::fprintf(stderr, "[FAIL] unreadable PNG %s\n", path.toUtf8().constData());
        return false;
    }
    if (!save_succeeded) {
        std::fprintf(stderr, "[WARN] Qt reported save failure but PNG verified: %s\n",
                     path.toUtf8().constData());
    }
    std::fprintf(stderr, "[OK] wrote %s (%dx%d)\n", path.toUtf8().constData(), image.width(),
                 image.height());
    return true;
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

bool send_object(QLocalSocket& socket, const QJsonObject& object) {
    const auto encoded = frame(QJsonDocument(object).toJson(QJsonDocument::Compact));
    return socket.write(encoded) == encoded.size() && socket.waitForBytesWritten(1'000);
}

QJsonObject response(const QString& request_id, const qint64 kind, const qint64 request_kind,
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

bool send_service_info(QLocalSocket& socket, const QString& request_id) {
    const QJsonArray capabilities{QStringLiteral("job.list"), QStringLiteral("repository.list"),
                                  QStringLiteral("service.info")};
    const QJsonObject service{{QStringLiteral("minimum_api_version"), 3},
                              {QStringLiteral("api_version"), 3},
                              {QStringLiteral("state"), 2},
                              {QStringLiteral("service_version"), QStringLiteral("0.1.0")},
                              {QStringLiteral("capabilities"), capabilities}};
    return send_object(socket,
                       response(request_id, 1, 1, 0, QStringLiteral("service.ready"), service));
}

bool send_not_configured(QLocalSocket& socket, const QString& request_id) {
    const QJsonObject catalog{{QStringLiteral("state"), 1},
                              {QStringLiteral("repository_uuid"), QString{}},
                              {QStringLiteral("items"), QJsonArray{}},
                              {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}};
    const QJsonObject page{
        {QStringLiteral("repository_connection_id"), QJsonValue(QJsonValue::Null)},
        {QStringLiteral("catalog"), catalog}};
    return send_object(
        socket, response(request_id, 1, 2, 0, QStringLiteral("repository.not_configured"), page));
}

QJsonObject job_summary(const QString& job_id, const qint64 state, const bool with_progress) {
    const auto message_code = state == 4   ? QStringLiteral("job.succeeded")
                              : state == 5 ? QStringLiteral("job.failed")
                                           : QStringLiteral("job.running");
    QJsonObject job{{QStringLiteral("job_id"), job_id},
                    {QStringLiteral("trace_id"), QStringLiteral("trace-") + job_id},
                    {QStringLiteral("operation"), 1},
                    {QStringLiteral("state"), state},
                    {QStringLiteral("created_utc_ms"), 1'775'174'400'000LL},
                    {QStringLiteral("started_utc_ms"), 1'775'174'400'100LL},
                    {QStringLiteral("completed_utc_ms"),
                     state >= 4 ? QJsonValue(1'775'174'401'000LL) : QJsonValue(QJsonValue::Null)},
                    {QStringLiteral("message_code"), message_code}};
    if (with_progress) {
        job.insert(QStringLiteral("progress"),
                   QJsonObject{{QStringLiteral("schema_version"), 1},
                               {QStringLiteral("job_id"), job_id},
                               {QStringLiteral("trace_id"), QStringLiteral("trace-") + job_id},
                               {QStringLiteral("phase"), 2},
                               {QStringLiteral("logical_bytes"), 1'000},
                               {QStringLiteral("processed_bytes"), 400},
                               {QStringLiteral("stored_bytes"), 300},
                               {QStringLiteral("message_code"), QStringLiteral("job.running")}});
    } else {
        job.insert(QStringLiteral("progress"), QJsonValue(QJsonValue::Null));
    }
    return job;
}

bool send_job_page(QLocalSocket& socket, const QString& request_id) {
    const QJsonArray items{job_summary(QStringLiteral("job-run"), 2, true),
                           job_summary(QStringLiteral("job-ok"), 4, false),
                           job_summary(QStringLiteral("job-fail"), 5, false)};
    const QJsonObject payload{{QStringLiteral("items"), items},
                              {QStringLiteral("continuation_token"), QJsonValue(QJsonValue::Null)}};
    return send_object(socket,
                       response(request_id, 1, 5, 0, QStringLiteral("job.list_ready"), payload));
}

QByteArray& socket_inbox(QLocalSocket& socket) {
    static QHash<QLocalSocket*, QByteArray> inboxes;
    return inboxes[&socket];
}

[[nodiscard]] QByteArray receive_frame(QLocalSocket& socket, const int timeout_ms) {
    auto& input = socket_inbox(socket);
    quint32 expected = 0;
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        input.append(socket.readAll());
        if (expected == 0) {
            if (input.size() < 4) {
                socket.waitForReadyRead(20);
                continue;
            }
            expected = static_cast<quint32>(static_cast<unsigned char>(input[0])) |
                       (static_cast<quint32>(static_cast<unsigned char>(input[1])) << 8U) |
                       (static_cast<quint32>(static_cast<unsigned char>(input[2])) << 16U) |
                       (static_cast<quint32>(static_cast<unsigned char>(input[3])) << 24U);
            input.remove(0, 4);
        }
        if (expected > 0 && input.size() >= static_cast<int>(expected)) {
            const auto body = input.left(static_cast<int>(expected));
            input.remove(0, static_cast<int>(expected));
            return body;
        }
        socket.waitForReadyRead(20);
    }
    return {};
}

[[nodiscard]] QJsonObject receive_object(QLocalSocket& socket, const int timeout_ms) {
    const auto document = QJsonDocument::fromJson(receive_frame(socket, timeout_ms));
    return document.isObject() ? document.object() : QJsonObject{};
}

[[nodiscard]] bool wait_until(const std::function<bool()>& predicate, const int timeout_ms) {
    QElapsedTimer elapsed;
    elapsed.start();
    while (elapsed.elapsed() < timeout_ms) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
        if (predicate()) {
            return true;
        }
    }
    return predicate();
}

// Serve one Ready session with sample jobs so Home is visible under splash dismissal.
[[nodiscard]] bool serve_ready_session(QLocalServer& server, aegra::desktop::ServiceClient& client,
                                       QLocalSocket*& socket) {
    socket = nullptr;
    if (!wait_until([&] { return server.hasPendingConnections(); }, 5'000)) {
        return false;
    }
    socket = server.nextPendingConnection();
    if (socket == nullptr) {
        return false;
    }
    const auto info = receive_object(*socket, 3'000);
    if (info.isEmpty() ||
        !send_service_info(*socket, info.value(QStringLiteral("request_id")).toString())) {
        return false;
    }
    if (!wait_until([&] { return client.connected(); }, 5'000)) {
        return false;
    }
    QJsonObject repo;
    QJsonObject job;
    QElapsedTimer collect;
    collect.start();
    while (collect.elapsed() < 5'000 && (repo.isEmpty() || job.isEmpty())) {
        const auto request = receive_object(*socket, 500);
        if (request.isEmpty()) {
            continue;
        }
        const auto kind = request.value(QStringLiteral("kind")).toInteger();
        if (kind == 2) {
            repo = request;
        } else if (kind == 5) {
            job = request;
        }
    }
    if (repo.isEmpty() || job.isEmpty()) {
        return false;
    }
    return send_not_configured(*socket, repo.value(QStringLiteral("request_id")).toString()) &&
           send_job_page(*socket, job.value(QStringLiteral("request_id")).toString()) &&
           wait_until(
               [&] {
                   return client.connected() && !client.splashVisible() &&
                          client.jobs()->rowCount() == 3;
               },
               5'000);
}

[[nodiscard]] bool capture_matrix(QQuickWindow* window, const QString& out_root,
                                  const QString& phase_prefix,
                                  aegra::desktop::LocaleController& locale_controller) {
    struct Viewport {
        int width;
        int height;
        const char* label;
    };
    const std::vector<Viewport> viewports{{900, 600, "900x600"}, {1080, 720, "1080x720"}};
    const QStringList locales{QStringLiteral("en_US"), QStringLiteral("zh_CN"),
                              QStringLiteral("zh_TW"), QStringLiteral("ja_JP"),
                              QStringLiteral("de_DE")};
    bool passed = true;
    for (const auto& locale : locales) {
        passed &= expect(locale_controller.setLanguage(locale), "set language");
        process_for(120);
        for (const auto& viewport : viewports) {
            window->resize(viewport.width, viewport.height);
            process_for(120);
            const auto path = QDir(out_root).filePath(
                QStringLiteral("%1_%2_%3.png")
                    .arg(phase_prefix, locale, QLatin1String(viewport.label)));
            passed &= expect(capture_window(window, path), "viewport capture");
            passed &=
                expect(window->width() == viewport.width && window->height() == viewport.height,
                       "window size applied");
            passed &= expect(window->minimumWidth() == 900 && window->minimumHeight() == 600,
                             "minimum window size remains 900x600");
        }
    }
    // 150% of 1080x720 logical size (host DPI may scale physical pixels further).
    window->resize(1620, 1080);
    process_for(120);
    passed &=
        expect(capture_window(
                   window, QDir(out_root).filePath(
                               QStringLiteral("%1_en_US_150pct_1620x1080.png").arg(phase_prefix))),
               "150 percent scale capture");
    return passed;
}

} // namespace

int main(int argument_count, char* arguments[]) {
    // Prefer native Windows platform for real font glyphs; offscreen is CI fallback.
    if (qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) {
#ifdef Q_OS_WIN
        qputenv("QT_QPA_PLATFORM", "windows");
#else
        qputenv("QT_QPA_PLATFORM", "offscreen");
#endif
    }

    QGuiApplication application(argument_count, arguments);
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    application.setOrganizationName(QStringLiteral("Aegra"));
    application.setApplicationName(QStringLiteral("AegraVisualSmoke"));

    QLocalServer::removeServer(QLatin1String(kPipeName));
    QLocalServer server;
    // Phase 1: no Service → splash failure / Retry (capture before accepting).
    QQmlApplicationEngine engine;
    aegra::desktop::LocaleController locale_controller(&engine);
    aegra::desktop::ServiceClient service_client;
    service_client.set_locale_controller(&locale_controller);

    engine.addImportPath(QStringLiteral("qrc:/Aegra/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("localeController"),
                                             &locale_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("serviceClient"), &service_client);

    const QUrl root(QStringLiteral("qrc:/Aegra/qml/Main.qml"));
    engine.load(root);
    if (engine.rootObjects().isEmpty()) {
        std::fputs("[FAIL] Main.qml failed to load\n", stderr);
        return EXIT_FAILURE;
    }

    auto* window = qobject_cast<QQuickWindow*>(engine.rootObjects().constFirst());
    if (window == nullptr) {
        std::fputs("[FAIL] root object is not QQuickWindow\n", stderr);
        return EXIT_FAILURE;
    }

    window->setFlags(window->flags() | Qt::FramelessWindowHint);
    window->show();
    process_for(300);
    // Wait until splash settles into failure (connect failed) or remains busy.
    if (!wait_until([&] { return service_client.splashVisible(); }, 2'000)) {
        std::fputs("[FAIL] splash never became visible\n", stderr);
        return EXIT_FAILURE;
    }
    process_for(500);

    const auto out_root = evidence_root();
    bool passed = expect(QDir().mkpath(out_root), "evidence output directory created");
    passed &= capture_matrix(window, out_root, QStringLiteral("splash"), locale_controller);

    // Phase 2: mock Service → Ready Home with jobs, progress, disabled Backup/Restore/Mount.
    if (!server.listen(QLatin1String(kPipeName))) {
        std::fputs("[FAIL] mock service listener\n", stderr);
        return EXIT_FAILURE;
    }
    service_client.reconnect();
    QLocalSocket* socket = nullptr;
    passed &=
        expect(serve_ready_session(server, service_client, socket), "mock service ready with jobs");
    process_for(200);
    passed &= capture_matrix(window, out_root, QStringLiteral("home"), locale_controller);

    if (socket != nullptr) {
        socket->abort();
    }
    server.close();
    QLocalServer::removeServer(QLatin1String(kPipeName));

    if (!passed) {
        return EXIT_FAILURE;
    }
    std::fprintf(stderr, "[OK] D2 visual smoke evidence written under %s\n",
                 out_root.toUtf8().constData());
    return EXIT_SUCCESS;
}
