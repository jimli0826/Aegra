#include "client/models/recovery_point_model.h"

#include "locale/locale_format.h"

#include <QDateTime>
#include <QSet>
#include <QTimeZone>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace aegra::desktop {
namespace {

[[nodiscard]] QDateTime local_from_utc_ms(const std::int64_t created_utc_ms) {
    return QDateTime::fromMSecsSinceEpoch(created_utc_ms, QTimeZone::UTC).toLocalTime();
}

} // namespace

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
    case ParentSummaryTextRole:
        return parent_summary_text(row);
    case BackupTypeTextRole:
        return backup_type_text(row.backup_type);
    case ContentKindRole:
        return static_cast<qint64>(row.content_kind);
    case ChainCompleteRole:
        return row.chain_state == 1;
    case ChainStateTextRole:
        return chain_state_text(row.chain_state);
    case ChainDepthRole:
        return chain_depth_for(row);
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
    case DeduplicatedBlockCountRole:
        return static_cast<qint64>(row.deduplicated_block_count);
    case DeduplicatedLogicalBytesRole:
        return static_cast<qint64>(row.deduplicated_logical_bytes);
    case DeduplicatedLogicalBytesTextRole:
        return format_ != nullptr ? format_->format_bytes(row.deduplicated_logical_bytes)
                                  : QString{};
    case SourceCountRole:
        return static_cast<qint64>(row.source_count);
    case HasSidecarRole:
        return row.has_sidecar;
    case IsBaselineRole:
        return row.backup_type == 1;
    default:
        return {};
    }
}

QHash<int, QByteArray> RecoveryPointModel::roleNames() const {
    return {
        {FileUuidRole, "fileUuid"},
        {BackupSetUuidRole, "backupSetUuid"},
        {ParentUuidRole, "parentUuid"},
        {ParentSummaryTextRole, "parentSummaryText"},
        {BackupTypeTextRole, "backupTypeText"},
        {ContentKindRole, "contentKind"},
        {ChainCompleteRole, "chainComplete"},
        {ChainStateTextRole, "chainStateText"},
        {ChainDepthRole, "chainDepth"},
        {CreatedUtcMsRole, "createdUtcMs"},
        {CreatedTextRole, "createdText"},
        {LogicalSizeBytesRole, "logicalSizeBytes"},
        {LogicalSizeTextRole, "logicalSizeText"},
        {StoredSizeBytesRole, "storedSizeBytes"},
        {StoredSizeTextRole, "storedSizeText"},
        {DeduplicatedBlockCountRole, "deduplicatedBlockCount"},
        {DeduplicatedLogicalBytesRole, "deduplicatedLogicalBytes"},
        {DeduplicatedLogicalBytesTextRole, "deduplicatedLogicalBytesText"},
        {SourceCountRole, "sourceCount"},
        {HasSidecarRole, "hasSidecar"},
        {IsBaselineRole, "isBaseline"},
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

QString RecoveryPointModel::short_uuid(const QString& uuid) {
    if (uuid.size() < 8) {
        return uuid;
    }
    return uuid.left(8);
}

const RecoveryPointRow* RecoveryPointModel::find_row(const QString& file_uuid) const {
    if (file_uuid.isEmpty()) {
        return nullptr;
    }
    for (const auto& row : rows_) {
        if (row.file_uuid == file_uuid) {
            return &row;
        }
    }
    return nullptr;
}

QString RecoveryPointModel::parent_summary_text(const RecoveryPointRow& row) const {
    if (row.parent_uuid.isEmpty()) {
        //% "Baseline (no parent)"
        return qtTrId("aegra.repository.parent.baseline");
    }
    const auto* parent = find_row(row.parent_uuid);
    const auto short_id = short_uuid(row.parent_uuid);
    if (parent == nullptr) {
        //% "Parent %1"
        return qtTrId("aegra.repository.parent.id_only").arg(short_id);
    }
    const auto time_text =
        format_ != nullptr ? format_->format_date_time_utc_ms(parent->created_utc_ms)
                           : local_time_hm(parent->created_utc_ms);
    //% "Parent %1 · %2"
    return qtTrId("aegra.repository.parent.summary").arg(short_id, time_text);
}

int RecoveryPointModel::chain_depth_for(const RecoveryPointRow& row) const {
    // Display-only walk of loaded list (tip inclusive). Not authority for delete/restore.
    int depth = 1;
    QString cursor = row.parent_uuid;
    QSet<QString> seen;
    seen.insert(row.file_uuid);
    while (!cursor.isEmpty() && !seen.contains(cursor) && depth < 128) {
        seen.insert(cursor);
        ++depth;
        const auto* parent = find_row(cursor);
        if (parent == nullptr) {
            break;
        }
        cursor = parent->parent_uuid;
    }
    return depth;
}

QVariantMap RecoveryPointModel::recoveryPointDetails(const QString& file_uuid) const {
    const auto* row = find_row(file_uuid);
    if (row == nullptr) {
        return {};
    }
    return {{QStringLiteral("fileUuid"), row->file_uuid},
            {QStringLiteral("backupSetUuid"), row->backup_set_uuid},
            {QStringLiteral("backupTypeText"), backup_type_text(row->backup_type)},
            {QStringLiteral("contentKind"), static_cast<qint64>(row->content_kind)},
            {QStringLiteral("createdText"),
             format_ != nullptr ? format_->format_date_time_utc_ms(row->created_utc_ms) : QString{}},
            {QStringLiteral("logicalSizeText"),
             format_ != nullptr ? format_->format_bytes(row->logical_size_bytes) : QString{}},
            {QStringLiteral("storedSizeText"),
             format_ != nullptr ? format_->format_bytes(row->stored_size_bytes) : QString{}},
            {QStringLiteral("deduplicatedBlockCount"),
             static_cast<qint64>(row->deduplicated_block_count)},
            {QStringLiteral("deduplicatedLogicalBytesText"),
             format_ != nullptr ? format_->format_bytes(row->deduplicated_logical_bytes)
                                : QString{}},
            {QStringLiteral("chainComplete"), row->chain_state == 1},
            {QStringLiteral("chainStateText"), chain_state_text(row->chain_state)},
            {QStringLiteral("chainDepth"), chain_depth_for(*row)},
            {QStringLiteral("parentSummaryText"), parent_summary_text(*row)},
            {QStringLiteral("isBaseline"), row->backup_type == 1},
            {QStringLiteral("sourceCount"), static_cast<qint64>(row->source_count)}};
}

QString RecoveryPointModel::local_date_ymd(const std::int64_t created_utc_ms) {
    if (created_utc_ms <= 0) {
        return {};
    }
    return local_from_utc_ms(created_utc_ms).date().toString(QStringLiteral("yyyy-MM-dd"));
}

QString RecoveryPointModel::local_time_hm(const std::int64_t created_utc_ms) {
    if (created_utc_ms <= 0) {
        return {};
    }
    return local_from_utc_ms(created_utc_ms).time().toString(QStringLiteral("HH:mm"));
}

QStringList RecoveryPointModel::backupDateYmds() const {
    QSet<QString> unique;
    for (const auto& row : rows_) {
        const auto ymd = local_date_ymd(row.created_utc_ms);
        if (!ymd.isEmpty()) {
            unique.insert(ymd);
        }
    }
    QStringList list = unique.values();
    std::sort(list.begin(), list.end());
    return list;
}

QVariantList RecoveryPointModel::checkpointsForDate(const QString& date_ymd) const {
    if (date_ymd.isEmpty()) {
        return {};
    }
    QVector<const RecoveryPointRow*> matches;
    matches.reserve(rows_.size());
    for (const auto& row : rows_) {
        if (local_date_ymd(row.created_utc_ms) == date_ymd) {
            matches.push_back(&row);
        }
    }
    std::sort(matches.begin(), matches.end(), [](const RecoveryPointRow* left,
                                                 const RecoveryPointRow* right) {
        return left->created_utc_ms > right->created_utc_ms;
    });
    QVariantList out;
    out.reserve(matches.size());
    for (const auto* row : matches) {
        QVariantMap map;
        map.insert(QStringLiteral("fileUuid"), row->file_uuid);
        map.insert(QStringLiteral("backupSetUuid"), row->backup_set_uuid);
        map.insert(QStringLiteral("timeText"), local_time_hm(row->created_utc_ms));
        map.insert(QStringLiteral("backupType"), backup_type_text(row->backup_type));
        map.insert(QStringLiteral("contentKind"), static_cast<qint64>(row->content_kind));
        map.insert(QStringLiteral("sizeText"),
                   format_ != nullptr ? format_->format_bytes(row->logical_size_bytes) : QString{});
        map.insert(QStringLiteral("logicalSizeBytes"), static_cast<qint64>(row->logical_size_bytes));
        map.insert(QStringLiteral("sourceCount"), static_cast<qint64>(row->source_count));
        map.insert(QStringLiteral("createdUtcMs"), static_cast<qint64>(row->created_utc_ms));
        map.insert(QStringLiteral("createdText"),
                   format_ != nullptr ? format_->format_date_time_utc_ms(row->created_utc_ms)
                                      : QString{});
        map.insert(QStringLiteral("chainComplete"), row->chain_state == 1);
        map.insert(QStringLiteral("parentSummary"), parent_summary_text(*row));
        map.insert(QStringLiteral("chainDepth"), chain_depth_for(*row));
        map.insert(QStringLiteral("isBaseline"), row->backup_type == 1);
        out.push_back(std::move(map));
    }
    return out;
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
        row.content_kind = map.value(QStringLiteral("contentKind"), 1).toLongLong();
        if (row.content_kind != 1 && row.content_kind != 2) {
            row.content_kind = 1;
        }
        row.chain_state = map.value(QStringLiteral("chainState")).toLongLong();
        row.created_utc_ms = map.value(QStringLiteral("createdUtcMs")).toLongLong();
        row.logical_size_bytes = map.value(QStringLiteral("logicalSizeBytes")).toLongLong();
        row.stored_size_bytes = map.value(QStringLiteral("storedSizeBytes")).toLongLong();
        row.deduplicated_block_count =
            map.value(QStringLiteral("deduplicatedBlockCount")).toLongLong();
        row.deduplicated_logical_bytes =
            map.value(QStringLiteral("deduplicatedLogicalBytes")).toLongLong();
        row.source_count = map.value(QStringLiteral("sourceCount")).toLongLong();
        row.has_sidecar = map.value(QStringLiteral("hasSidecar")).toBool();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
