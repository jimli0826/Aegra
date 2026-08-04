#include "locale/message_code_map.h"

#include <QHash>

namespace aegra::desktop {
namespace {

[[nodiscard]] const QHash<QString, QString>& catalog() {
    static const QHash<QString, QString> kCatalog{
        {QStringLiteral("service.ready"), QStringLiteral("aegra.service.message.ready")},
        {QStringLiteral("service.disconnected"),
         QStringLiteral("aegra.error.service.disconnected")},
        {QStringLiteral("service.connect_failed"),
         QStringLiteral("aegra.error.service.connect_failed")},
        {QStringLiteral("service.protocol_invalid"),
         QStringLiteral("aegra.error.service.protocol_invalid")},
        {QStringLiteral("service.request_timeout"),
         QStringLiteral("aegra.error.service.request_timeout")},
        {QStringLiteral("service.send_failed"), QStringLiteral("aegra.error.service.send_failed")},
        {QStringLiteral("repository.not_configured"),
         QStringLiteral("aegra.repository.status.not_configured")},
        {QStringLiteral("repository.catalog_ready"),
         QStringLiteral("aegra.repository.status.catalog_ready")},
        {QStringLiteral("repository.query_failed"),
         QStringLiteral("aegra.error.repository.query_failed")},
        {QStringLiteral("job.queued"), QStringLiteral("aegra.task.state.queued")},
        {QStringLiteral("job.running"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("job.progress"), QStringLiteral("aegra.task.state.running")},
        {QStringLiteral("job.cancelling"), QStringLiteral("aegra.task.state.cancelling")},
        {QStringLiteral("job.succeeded"), QStringLiteral("aegra.task.state.succeeded")},
        {QStringLiteral("job.failed"), QStringLiteral("aegra.task.state.failed")},
        {QStringLiteral("job.cancelled"), QStringLiteral("aegra.task.state.cancelled")},
        {QStringLiteral("job.interrupted"), QStringLiteral("aegra.task.state.interrupted")},
        {QStringLiteral("job.deadline_exceeded"),
         QStringLiteral("aegra.error.job.deadline_exceeded")},
        {QStringLiteral("job.query_failed"), QStringLiteral("aegra.error.job.query_failed")},
        {QStringLiteral("inventory.query_failed"),
         QStringLiteral("aegra.error.inventory.query_failed")},
        {QStringLiteral("connection.query_failed"),
         QStringLiteral("aegra.error.connection.query_failed")},
        {QStringLiteral("backup.command_failed"),
         QStringLiteral("aegra.error.backup.command_failed")},
        {QStringLiteral("backup.preflight_failed"),
         QStringLiteral("aegra.error.backup.preflight_failed")},
        {QStringLiteral("backup.repository_unavailable"),
         QStringLiteral("aegra.error.backup.repository_unavailable")},
        {QStringLiteral("backup.source_not_selectable"),
         QStringLiteral("aegra.error.backup.source_not_selectable")},
        {QStringLiteral("backup.source_not_found"),
         QStringLiteral("aegra.error.backup.source_not_found")},
        {QStringLiteral("backup.worker_unavailable"),
         QStringLiteral("aegra.error.backup.worker_unavailable")},
        {QStringLiteral("backup.idempotency_conflict"),
         QStringLiteral("aegra.error.backup.idempotency_conflict")},
        {QStringLiteral("service.request_failed"),
         QStringLiteral("aegra.error.service.request_failed")},
        {QStringLiteral("job.cancel_failed"), QStringLiteral("aegra.error.job.cancel_failed")},
        {QStringLiteral("command.accepted"),
         QStringLiteral("aegra.service.message.command_accepted")},
        {QStringLiteral("command.replayed"),
         QStringLiteral("aegra.service.message.command_replayed")},
        {QStringLiteral("control_plane.ready"), QStringLiteral("aegra.service.message.ready")},
    };
    return kCatalog;
}

} // namespace

QString translation_id_for_message_code(const QString& message_code) {
    const auto it = catalog().constFind(message_code);
    if (it == catalog().cend()) {
        return QStringLiteral("aegra.error.unknown");
    }
    return *it;
}

QString localize_message_code(const QString& message_code) {
    const auto id = translation_id_for_message_code(message_code);
    if (id == QLatin1String("aegra.error.unknown")) {
        //% "Unexpected service response (%1)"
        auto text = qtTrId("aegra.error.unknown");
        // Without a loaded QM, qtTrId returns the ID which has no %1 placeholder.
        if (!text.contains(QLatin1String("%1"))) {
            text = QStringLiteral("Unexpected service response (%1)");
        }
        return text.arg(message_code);
    }
    auto text = qtTrId(id.toUtf8().constData());
    if (text == id) {
        // Keep a deterministic English fallback when the pack is missing this ID.
        return id;
    }
    return text;
}

} // namespace aegra::desktop
