#include "client/models/recovery_point_model.h"

#include "locale/locale_format.h"

#include <utility>

namespace aegra::desktop {

RecoveryPointModel::RecoveryPointModel(QObject* parent) : QAbstractListModel(parent) {}

void RecoveryPointModel::set_locale_format(LocaleFormat* format) { format_ = format; }

void RecoveryPointModel::set_rows(QVector<RecoveryPointRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    endResetModel();
    emit countChanged();
}

void RecoveryPointModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    endResetModel();
    emit countChanged();
}

void RecoveryPointModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
}

int RecoveryPointModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid()) {
        return 0;
    }
    return rows_.size();
}

QVariant RecoveryPointModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_[index.row()];
    switch (role) {
    case FileUuidRole:
        return row.file_uuid;
    case BackupSetUuidRole:
        return row.backup_set_uuid;
    case ParentUuidRole:
        return row.parent_uuid;
    case BackupTypeTextRole:
        return backup_type_text(row.backup_type);
    case ChainCompleteRole:
        return row.chain_state == 1;
    case ChainStateTextRole:
        return chain_state_text(row.chain_state);
    case CreatedUtcMsRole:
        return static_cast<qint64>(row.created_utc_ms);
    case CreatedTextRole:
        return format_ != nullptr ? format_->format_date_time_utc_ms(row.created_utc_ms)
                                  : QString{};
    case LogicalSizeBytesRole:
        return static_cast<qint64>(row.logical_size_bytes);
    case LogicalSizeTextRole:
        return format_ != nullptr ? format_->format_bytes(row.logical_size_bytes) : QString{};
    case StoredSizeBytesRole:
        return static_cast<qint64>(row.stored_size_bytes);
    case StoredSizeTextRole:
        return format_ != nullptr ? format_->format_bytes(row.stored_size_bytes) : QString{};
    case SourceCountRole:
        return static_cast<qint64>(row.source_count);
    case HasSidecarRole:
        return row.has_sidecar;
    default:
        return {};
    }
}

QHash<int, QByteArray> RecoveryPointModel::roleNames() const {
    return {
        {FileUuidRole, "fileUuid"},
        {BackupSetUuidRole, "backupSetUuid"},
        {ParentUuidRole, "parentUuid"},
        {BackupTypeTextRole, "backupTypeText"},
        {ChainCompleteRole, "chainComplete"},
        {ChainStateTextRole, "chainStateText"},
        {CreatedUtcMsRole, "createdUtcMs"},
        {CreatedTextRole, "createdText"},
        {LogicalSizeBytesRole, "logicalSizeBytes"},
        {LogicalSizeTextRole, "logicalSizeText"},
        {StoredSizeBytesRole, "storedSizeBytes"},
        {StoredSizeTextRole, "storedSizeText"},
        {SourceCountRole, "sourceCount"},
        {HasSidecarRole, "hasSidecar"},
    };
}

QString RecoveryPointModel::backup_type_text(const std::int64_t backup_type) const {
    switch (backup_type) {
    case 1:
        //% "Full"
        return qtTrId("aegra.backup.type.full");
    case 2:
        //% "Incremental"
        return qtTrId("aegra.backup.type.incremental");
    case 3:
        //% "Differential"
        return qtTrId("aegra.backup.type.differential");
    default:
        //% "Unknown"
        return qtTrId("aegra.backup.type.unknown");
    }
}

QString RecoveryPointModel::chain_state_text(const std::int64_t chain_state) const {
    if (chain_state == 1) {
        //% "Complete"
        return qtTrId("aegra.repository.chain.complete");
    }
    //% "Incomplete"
    return qtTrId("aegra.repository.chain.incomplete");
}

QVector<RecoveryPointRow> recovery_points_from_variant_list(const QVariantList& items) {
    QVector<RecoveryPointRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const auto map = item.toMap();
        RecoveryPointRow row;
        row.file_uuid = map.value(QStringLiteral("fileUuid")).toString();
        row.backup_set_uuid = map.value(QStringLiteral("backupSetUuid")).toString();
        row.parent_uuid = map.value(QStringLiteral("parentUuid")).toString();
        row.backup_type = map.value(QStringLiteral("backupType")).toLongLong();
        row.chain_state = map.value(QStringLiteral("chainState")).toLongLong();
        row.created_utc_ms = map.value(QStringLiteral("createdUtcMs")).toLongLong();
        row.logical_size_bytes = map.value(QStringLiteral("logicalSizeBytes")).toLongLong();
        row.stored_size_bytes = map.value(QStringLiteral("storedSizeBytes")).toLongLong();
        row.source_count = map.value(QStringLiteral("sourceCount")).toLongLong();
        row.has_sidecar = map.value(QStringLiteral("hasSidecar")).toBool();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
