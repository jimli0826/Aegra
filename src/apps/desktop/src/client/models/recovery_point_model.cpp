#include "client/models/recovery_point_model.h"

#include "locale/locale_format.h"
#include "locale/message_code_map.h"

#include <QClipboard>
#include <QDateTime>
#include <QGuiApplication>
#include <QHash>
#include <QSet>
#include <QSettings>
#include <QStringList>
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
    assign_display_numbers();
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
    emit countChanged();
}

void RecoveryPointModel::assign_display_numbers() {
    display_numbers_.clear();
    QSettings settings;
    settings.beginGroup(QStringLiteral("recoveryPointLabels"));
    QVector<const RecoveryPointRow*> ordered;
    for (const auto& row : rows_) {
        ordered.push_back(&row);
    }
    std::sort(ordered.begin(), ordered.end(), [](const auto* left, const auto* right) {
        return left->created_utc_ms != right->created_utc_ms
                   ? left->created_utc_ms < right->created_utc_ms
                   : left->file_uuid < right->file_uuid;
    });
    for (const auto* row : ordered) {
        settings.beginGroup(row->backup_set_uuid);
        const auto key = QStringLiteral("points/") + row->file_uuid;
        auto number = settings.value(key).toULongLong();
        if (number == 0) {
            number = settings.value(QStringLiteral("lastNumber"), 0).toULongLong() + 1;
            settings.setValue(key, number);
            settings.setValue(QStringLiteral("lastNumber"), number);
        }
        display_numbers_.insert(row->file_uuid, number);
        settings.endGroup();
    }
}

QString RecoveryPointModel::point_title(const RecoveryPointRow& row) const {
    return qtTrId("aegra.repository.point.title").arg(display_numbers_.value(row.file_uuid));
}

void RecoveryPointModel::copyIdentifier(const QString& file_uuid) const {
    if (find_row(file_uuid) != nullptr) {
        QGuiApplication::clipboard()->setText(file_uuid);
    }
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

QString RecoveryPointModel::content_kind_text(const std::int64_t content_kind) const {
    if (content_kind == 2) {
        //% "File set"
        return qtTrId("aegra.repository.content.file_set");
    }
    //% "Volume set"
    return qtTrId("aegra.repository.content.volume_set");
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
    if (parent == nullptr) {
        return qtTrId("aegra.repository.point.parent_unavailable");
    }
    const auto time_text =
        format_ != nullptr ? format_->format_date_time_utc_ms(parent->created_utc_ms)
                           : local_time_hm(parent->created_utc_ms);
    return qtTrId("aegra.repository.point.based_on").arg(point_title(*parent), time_text);
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

int RecoveryPointModel::volumeSetCount() const {
    int n = 0;
    for (const auto& row : rows_) {
        if (row.content_kind == 1) {
            ++n;
        }
    }
    return n;
}

int RecoveryPointModel::fileSetCount() const {
    int n = 0;
    for (const auto& row : rows_) {
        if (row.content_kind == 2) {
            ++n;
        }
    }
    return n;
}

qint64 RecoveryPointModel::totalStoredBytes() const {
    qint64 total = 0;
    for (const auto& row : rows_) {
        total += row.stored_size_bytes;
    }
    return total;
}

qint64 RecoveryPointModel::totalLogicalBytes() const {
    qint64 total = 0;
    for (const auto& row : rows_) {
        total += row.logical_size_bytes;
    }
    return total;
}

QStringList RecoveryPointModel::fileUuids() const {
    QStringList ids;
    ids.reserve(rows_.size());
    for (const auto& row : rows_) {
        ids.push_back(row.file_uuid);
    }
    return ids;
}

QVariant RecoveryPointModel::check_variant(const std::optional<RecoveryPointCheckRow>& check) const {
    if (!check) {
        return {};
    }
    QString key = QStringLiteral("failed");
    if (check->state == 1) {
        key = QStringLiteral("succeeded");
    } else if (check->state == 3) {
        key = QStringLiteral("cancelled");
    }
    return QVariantMap{
        {QStringLiteral("key"), key},
        {QStringLiteral("state"), static_cast<qint64>(check->state)},
        {QStringLiteral("messageCode"), check->message_code},
        {QStringLiteral("messageText"),
         check->message_code.isEmpty() ? QString{} : localize_message_code(check->message_code)},
        {QStringLiteral("jobId"), check->job_id},
        {QStringLiteral("completedUtcMs"), static_cast<qint64>(check->completed_utc_ms)},
        {QStringLiteral("completedText"),
         format_ != nullptr ? format_->format_date_time_utc_ms(check->completed_utc_ms)
                            : QString{}}};
}

QVariantMap RecoveryPointModel::recoveryPointChecks(const QString& file_uuid) const {
    const auto* row = find_row(file_uuid);
    if (row == nullptr) {
        return {};
    }
    return {{QStringLiteral("verifyCheck"), check_variant(row->verify_check)},
            {QStringLiteral("bootCheck"), check_variant(row->boot_check)}};
}

QVariantMap RecoveryPointModel::list_item_from_row(const RecoveryPointRow& row) const {
    return {{QStringLiteral("fileUuid"), row.file_uuid},
            {QStringLiteral("displayTitle"), point_title(row)},
            {QStringLiteral("backupSetUuid"), row.backup_set_uuid},
            {QStringLiteral("createdText"),
             format_ != nullptr ? format_->format_date_time_utc_ms(row.created_utc_ms) : QString{}},
            {QStringLiteral("backupTypeText"), backup_type_text(row.backup_type)},
            {QStringLiteral("logicalSizeText"),
             format_ != nullptr ? format_->format_bytes(row.logical_size_bytes) : QString{}},
            {QStringLiteral("storedSizeText"),
             format_ != nullptr ? format_->format_bytes(row.stored_size_bytes) : QString{}},
            {QStringLiteral("deduplicatedLogicalBytesText"),
             format_ != nullptr ? format_->format_bytes(row.deduplicated_logical_bytes)
                                : QString{}},
            {QStringLiteral("chainStateText"), chain_state_text(row.chain_state)},
            {QStringLiteral("chainComplete"), row.chain_state == 1},
            // 1 volume_set, 2 file_set: the Repository page gates Boot Check selection on it.
            {QStringLiteral("contentKind"), static_cast<qint64>(row.content_kind)},
            {QStringLiteral("parentSummaryText"), parent_summary_text(row)},
            {QStringLiteral("chainDepth"), chain_depth_for(row)},
            {QStringLiteral("isBaseline"), row.backup_type == 1},
            {QStringLiteral("verifyCheck"), check_variant(row.verify_check)},
            {QStringLiteral("bootCheck"), check_variant(row.boot_check)}};
}

QHash<QString, QStringList> RecoveryPointModel::children_by_parent() const {
    QHash<QString, QStringList> children;
    for (const auto& row : rows_) {
        if (!row.parent_uuid.isEmpty()) {
            children[row.parent_uuid].push_back(row.file_uuid);
        }
    }
    return children;
}

QVariantList RecoveryPointModel::backupSets() const {
    struct Accumulator final {
        int count{0};
        std::int64_t latest_utc_ms{0};
        std::int64_t content_kind{1};
        std::int64_t source_count{0};
        bool all_complete{true};
    };
    QHash<QString, Accumulator> by_set;
    QStringList order;
    for (const auto& row : rows_) {
        if (!by_set.contains(row.backup_set_uuid)) {
            order.push_back(row.backup_set_uuid);
            by_set.insert(row.backup_set_uuid, {});
        }
        auto& acc = by_set[row.backup_set_uuid];
        ++acc.count;
        if (row.created_utc_ms >= acc.latest_utc_ms) {
            acc.latest_utc_ms = row.created_utc_ms;
            acc.content_kind = row.content_kind;
            acc.source_count = row.source_count;
        }
        if (row.chain_state != 1) {
            acc.all_complete = false;
        }
    }
    std::sort(order.begin(), order.end(), [&](const QString& left, const QString& right) {
        return by_set.value(left).latest_utc_ms > by_set.value(right).latest_utc_ms;
    });
    QVariantList out;
    out.reserve(order.size());
    for (const auto& key : order) {
        const auto& acc = by_set[key];
        out.push_back(QVariantMap{
            {QStringLiteral("backupSetUuid"), key},
            {QStringLiteral("shortId"), short_uuid(key)},
            {QStringLiteral("recoveryPointCount"), acc.count},
            {QStringLiteral("latestCreatedText"),
             format_ != nullptr ? format_->format_date_time_utc_ms(acc.latest_utc_ms) : QString{}},
            {QStringLiteral("contentKind"), acc.content_kind},
            {QStringLiteral("contentKindText"), content_kind_text(acc.content_kind)},
            {QStringLiteral("sourceSummary"),
             qtTrId("aegra.repository.point.sources").arg(acc.source_count)},
            {QStringLiteral("chainComplete"), acc.all_complete}});
    }
    return out;
}

QVariantList RecoveryPointModel::recoveryPointsInSet(const QString& backup_set_uuid) const {
    QVector<const RecoveryPointRow*> matches;
    matches.reserve(rows_.size());
    for (const auto& row : rows_) {
        if (row.backup_set_uuid == backup_set_uuid) {
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
        auto item = list_item_from_row(*row);
        item.insert(QStringLiteral("isLatest"), row == matches.front());
        out.push_back(std::move(item));
    }
    return out;
}

QStringList RecoveryPointModel::fileUuidsInSet(const QString& backup_set_uuid) const {
    QStringList ids;
    for (const auto& row : rows_) {
        if (row.backup_set_uuid == backup_set_uuid) {
            ids.push_back(row.file_uuid);
        }
    }
    return ids;
}

QList<QStringList> RecoveryPointModel::groupFileUuidsByBackupSetChronological(
    const QStringList& file_uuids) const {
    QHash<QString, QVector<const RecoveryPointRow*>> by_set;
    QStringList set_order;
    QSet<QString> wanted;
    for (const auto& id : file_uuids) {
        wanted.insert(id);
    }
    for (const auto& row : rows_) {
        if (!wanted.contains(row.file_uuid)) {
            continue;
        }
        if (!by_set.contains(row.backup_set_uuid)) {
            set_order.push_back(row.backup_set_uuid);
        }
        by_set[row.backup_set_uuid].push_back(&row);
    }
    QList<QStringList> groups;
    groups.reserve(set_order.size());
    for (const auto& set_id : set_order) {
        auto members = by_set.value(set_id);
        std::sort(members.begin(), members.end(),
                  [](const RecoveryPointRow* left, const RecoveryPointRow* right) {
                      if (left->created_utc_ms != right->created_utc_ms) {
                          return left->created_utc_ms < right->created_utc_ms;
                      }
                      return left->file_uuid < right->file_uuid;
                  });
        QStringList ids;
        ids.reserve(members.size());
        for (const auto* row : members) {
            ids.push_back(row->file_uuid);
        }
        groups.push_back(std::move(ids));
    }
    return groups;
}

QStringList RecoveryPointModel::descendantFileUuids(const QString& file_uuid) const {
    if (file_uuid.isEmpty() || find_row(file_uuid) == nullptr) {
        return {};
    }
    const auto children = children_by_parent();
    QStringList ordered;
    QSet<QString> seen;
    QStringList stack{file_uuid};
    while (!stack.isEmpty()) {
        const auto current = stack.takeLast();
        if (seen.contains(current)) {
            continue;
        }
        seen.insert(current);
        ordered.push_back(current);
        const auto kids = children.value(current);
        for (auto it = kids.crbegin(); it != kids.crend(); ++it) {
            stack.push_back(*it);
        }
    }
    return ordered;
}

QStringList RecoveryPointModel::ancestorFileUuids(const QString& file_uuid) const {
    QStringList ancestors;
    QSet<QString> seen;
    seen.insert(file_uuid);
    const auto* row = find_row(file_uuid);
    while (row != nullptr && !row->parent_uuid.isEmpty() && ancestors.size() < 128) {
        if (seen.contains(row->parent_uuid)) {
            break;
        }
        seen.insert(row->parent_uuid);
        ancestors.push_back(row->parent_uuid);
        row = find_row(row->parent_uuid);
    }
    return ancestors;
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

namespace {

[[nodiscard]] std::optional<RecoveryPointCheckRow> check_row_from_variant(const QVariant& value) {
    if (!value.isValid() || !value.canConvert<QVariantMap>()) {
        return std::nullopt;
    }
    const auto map = value.toMap();
    RecoveryPointCheckRow check;
    check.state = map.value(QStringLiteral("state")).toLongLong();
    if (check.state < 1 || check.state > 4) {
        return std::nullopt;
    }
    check.message_code = map.value(QStringLiteral("messageCode")).toString();
    check.job_id = map.value(QStringLiteral("jobId")).toString();
    check.completed_utc_ms = map.value(QStringLiteral("completedUtcMs")).toLongLong();
    return check;
}

} // namespace

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
        row.verify_check = check_row_from_variant(map.value(QStringLiteral("verifyCheck")));
        row.boot_check = check_row_from_variant(map.value(QStringLiteral("bootCheck")));
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
