#include "client/service_client.h"
#include "locale/locale_controller.h"

#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQmlError>
#include <QQuickStyle>
#include <QSize>
#include <QStandardPaths>
#include <QTextStream>
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

} // namespace

int main(int argument_count, char* arguments[]) {
    QGuiApplication application(argument_count, arguments);
    configure_application(application);

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
    return engine.rootObjects().isEmpty() ? EXIT_FAILURE : application.exec();
}
