#include "client/service_client.h"
#include "locale/locale_controller.h"

#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QLibraryInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QQuickWindow>
#include <QSize>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QTextStream>
#include <QUrl>
#include <QWindow>

#if defined(Q_OS_WIN)
#  include <dwmapi.h>
#  ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#    define DWMWA_WINDOW_CORNER_PREFERENCE 33
#  endif
#  ifndef DWMWCP_ROUND
#    define DWMWCP_ROUND 2
#  endif
#endif

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

/// Enable per-pixel alpha so a frameless QML shell can show true rounded corners.
void configure_transparent_quick_surface() {
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    format.setAlphaBufferSize(8);
    QSurfaceFormat::setDefaultFormat(format);
    QQuickWindow::setDefaultAlphaBuffer(true);
}

/// Windows DWM corner preference: set DWMWCP_ROUND (2) for rounded window borders.
void apply_frameless_platform_chrome(QWindow* window) {
    if (window == nullptr) {
        return;
    }
    if (auto* quick = qobject_cast<QQuickWindow*>(window); quick != nullptr) {
        quick->setColor(Qt::transparent);
    }
#if defined(Q_OS_WIN)
    const HWND hwnd = reinterpret_cast<HWND>(window->winId());
    if (hwnd == nullptr) {
        return;
    }
    const int preference = DWMWCP_ROUND;
    DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &preference,
                          sizeof(preference));
#endif
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
    // Reclaim lock after crash (stale holder). Short stale window so a failed
    // previous run does not block the next double-click for long.
    lock_file.setStaleLockTime(3000);
    if (lock_file.tryLock(200)) {
        return true;
    }

    // Live primary: ask it to raise, then this process exits.
    QLocalSocket socket;
    socket.connectToServer(QLatin1String(kSingleInstanceServerName));
    if (socket.waitForConnected(800)) {
        socket.write("raise\n");
        socket.flush();
        socket.waitForBytesWritten(500);
        socket.disconnectFromServer();
        return false;
    }

    // Lock held but no IPC server — previous process died without cleanup.
    lock_file.removeStaleLockFile();
    QLocalServer::removeServer(QLatin1String(kSingleInstanceServerName));
    if (lock_file.tryLock(500)) {
        return true;
    }

    qWarning("Aegra desktop: could not acquire single-instance lock (%s)",
             qPrintable(lock_file.fileName()));
    return false;
}

} // namespace

int main(int argument_count, char* arguments[]) {
    // Must run before any QQuickWindow is created (alpha corners on Windows).
    configure_transparent_quick_surface();

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
    // Ensure Qt QML modules resolve when not launched from Qt Creator.
    engine.addImportPath(QLibraryInfo::path(QLibraryInfo::QmlImportsPath));
    QCoreApplication::addLibraryPath(QLibraryInfo::path(QLibraryInfo::PluginsPath));

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
                return;
            }
            if (object != nullptr && object_url == root) {
                apply_frameless_platform_chrome(qobject_cast<QWindow*>(object));
            }
        },
        Qt::QueuedConnection);
    engine.load(root);
    if (engine.rootObjects().isEmpty()) {
        return EXIT_FAILURE;
    }
    for (QObject* object : engine.rootObjects()) {
        apply_frameless_platform_chrome(qobject_cast<QWindow*>(object));
    }

    QObject::connect(&ipc_server, &QLocalServer::newConnection, &application, [&]() {
        while (QLocalSocket* client = ipc_server.nextPendingConnection()) {
            client->deleteLater();
            raise_main_windows(engine);
        }
    });

    return application.exec();
}
