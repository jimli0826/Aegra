#include "service_client.h"

#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QSize>
#include <QUrl>

namespace {

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

} // namespace

int main(int argument_count, char* arguments[]) {
    QGuiApplication application(argument_count, arguments);
    configure_application(application);

    ServiceClient service_client;
    QQmlApplicationEngine engine;
    engine.addImportPath(QStringLiteral("qrc:/Aegra/qml"));
    engine.rootContext()->setContextProperty(QStringLiteral("serviceClient"), &service_client);
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
    return engine.rootObjects().isEmpty() ? EXIT_FAILURE : application.exec();
}
