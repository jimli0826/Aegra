#include "client/service_client.h"
#include "locale/locale_controller.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QSize>
#include <QStandardPaths>
#include <QTextStream>
#include <QUrl>
#include <QWindow>

namespace {

// Per-user lock + local socket (same pattern as old AegraImage GUI).
constexpr char kSingleInstanceLockName[] = "Aegra.Desktop.singleinstance.lock";
constexpr char kSingleInstanceServerName[] = "Aegra.Desktop.IPC";

[[nodiscard]] QIcon load_product_icon() {
    QIcon icon;
    icon.addFile(QStringLiteral(":/Aegra/icons/product_32.png"), QSize(32, 32));
    icon.addFile(QStringLiteral(":/Aegra/icons/product_64.png"), QSize(64, 64));
    icon.addFile(QStringLiteral(":/Aegra/icons/product.png"), QSize(256, 256));
    return icon;
}

void configure_application(QGuiApplication& application) {
    QQuickStyle::setStyle(QStringLiteral("Basic"));
    application.setOrganizationName(QStringLiteral("Aegra"));
    application.setOrganizationDomain(QStringLiteral("aegra.app"));
    application.setApplicationName(QStringLiteral("Aegra"));
    const auto icon = load_product_icon();
    if (!icon.isNull()) {
        application.setWindowIcon(icon);
    }
}

void write_qml_errors(const QList<QQmlError>& errors) {
    if (errors.isEmpty()) {
        return;
    }
    const auto dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QFile file(dir + QStringLiteral("/aegra_desktop_qml_errors.txt"));
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        return;
    }
    QTextStream stream(&file);
    for (const auto& error : errors) {
        stream << error.toString() << '\n';
    }
}

void raise_main_windows(QQmlApplicationEngine& engine) {
    for (QObject* object : engine.rootObjects()) {
        auto* window = qobject_cast<QWindow*>(object);
        if (window == nullptr) {
            continue;
        }
        if (window->windowState() & Qt::WindowMinimized) {
            window->showNormal();
        } else {
            window->show();
        }
        window->raise();
        window->requestActivate();
    }
}

/// True if this process is the primary UI. Secondary instance asks primary to raise and exits.
[[nodiscard]] bool acquire_single_instance(QLockFile& lock_file) {
    // Reclaim lock after crash (stale holder).
    lock_file.setStaleLockTime(10000);
    if (lock_file.tryLock(200)) {
        return true;
    }
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kSingleInstanceServerName));
    if (socket.waitForConnected(800)) {
        socket.write("raise\n");
        socket.flush();
        socket.waitForBytesWritten(500);
        socket.disconnectFromServer();
    }
    return false;
}

} // namespace

int main(int argument_count, char* arguments[]) {
    QGuiApplication application(argument_count, arguments);
    configure_application(application);

    const QString lock_path =
        QDir(QStandardPaths::writableLocation(QStandardPaths::TempLocation))
            .filePath(QLatin1String(kSingleInstanceLockName));
    QLockFile single_instance_lock(lock_path);
    if (!acquire_single_instance(single_instance_lock)) {
        return 0;
    }

    QLocalServer::removeServer(QLatin1String(kSingleInstanceServerName));
    QLocalServer ipc_server;
    ipc_server.setSocketOptions(QLocalServer::UserAccessOption);
    if (!ipc_server.listen(QLatin1String(kSingleInstanceServerName))) {
        // Non-fatal: lock still enforces single instance; raise IPC may fail.
        qWarning("Single-instance IPC server failed to listen: %s",
                 qPrintable(ipc_server.errorString()));
    }

    QQmlApplicationEngine engine;
    aegra::desktop::LocaleController locale_controller(&engine);
    aegra::desktop::ServiceClient service_client;
    service_client.set_locale_controller(&locale_controller);

    engine.addImportPath(QStringLiteral("qrc:/Aegra/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("localeController"),
                                             &locale_controller);
    engine.rootContext()->setContextProperty(QStringLiteral("serviceClient"), &service_client);
    QObject::connect(&engine, &QQmlApplicationEngine::warnings, &application, &write_qml_errors);
    const QUrl root(QStringLiteral("qrc:/Aegra/qml/Main.qml"));
    QObject::connect(
        &engine, &QQmlApplicationEngine::objectCreated, &application,
        [root](QObject* object, const QUrl& object_url) {
            if (object == nullptr && object_url == root) {
                QCoreApplication::exit(EXIT_FAILURE);
            }
        },
        Qt::QueuedConnection);
    engine.load(root);
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }

    QObject::connect(&ipc_server, &QLocalServer::newConnection, &application, [&]() {
        while (QLocalSocket* client = ipc_server.nextPendingConnection()) {
            client->deleteLater();
            raise_main_windows(engine);
        }
    });

    return application.exec();
}
