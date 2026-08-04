#include "client/models/source_inventory_model.h"

#include "locale/locale_format.h"

#include <QVariantMap>

#include <utility>

namespace aegra::desktop {

SourceInventoryModel::SourceInventoryModel(QObject* parent) : QAbstractListModel(parent) {}

void SourceInventoryModel::set_locale_format(LocaleFormat* format) { format_ = format; }

void SourceInventoryModel::set_rows(QVector<SourceInventoryRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    selectable_count_ = 0;
    for (const auto& row : rows_) {
        if (row.is_selectable) {
            ++selectable_count_;
        }
    }
    endResetModel();
    emit countChanged();
}

void SourceInventoryModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    selectable_count_ = 0;
    endResetModel();
    emit countChanged();
}

void SourceInventoryModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
}

int SourceInventoryModel::selectableCount() const noexcept { return selectable_count_; }

bool SourceInventoryModel::contains_selectable(const QString& source_id) const {
    for (const auto& row : rows_) {
        if (row.source_id == source_id && row.is_selectable) {
            return true;
        }
    }
    return false;
}

std::optional<SourceInventoryRow> SourceInventoryModel::find(const QString& source_id) const {
    for (const auto& row : rows_) {
        if (row.source_id == source_id) {
            return row;
        }
    }
    return std::nullopt;
}

int SourceInventoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant SourceInventoryModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case SourceIdRole:
        return row.source_id;
    case DisplayNameRole:
        return row.display_name;
    case CapacityBytesRole:
        return static_cast<qint64>(row.capacity_bytes);
    case CapacityTextRole:
        return format_ != nullptr ? format_->format_bytes(row.capacity_bytes) : QString{};
    case AvailabilityTextRole:
        return availability_text(row);
    case IsSystemRole:
        return row.is_system;
    case IsReadOnlyRole:
        return row.is_read_only;
    case IsSelectableRole:
        return row.is_selectable;
    case DisabledReasonTextRole:
        return disabled_reason_text(row);
    default:
        return {};
    }
}

QHash<int, QByteArray> SourceInventoryModel::roleNames() const {
    return {{SourceIdRole, "sourceId"},
            {DisplayNameRole, "displayName"},
            {CapacityBytesRole, "capacityBytes"},
            {CapacityTextRole, "capacityText"},
            {AvailabilityTextRole, "availabilityText"},
            {IsSystemRole, "isSystem"},
            {IsReadOnlyRole, "isReadOnly"},
            {IsSelectableRole, "isSelectable"},
            {DisabledReasonTextRole, "disabledReasonText"}};
}

QString SourceInventoryModel::availability_text(const SourceInventoryRow& row) const {
    if (row.availability == 1) {
        //% "Available"
        return qtTrId("aegra.backup.source.available");
    }
    //% "Unavailable"
    return qtTrId("aegra.backup.source.unavailable");
}

QString SourceInventoryModel::disabled_reason_text(const SourceInventoryRow& row) const {
    if (row.is_selectable) {
        return {};
    }
    if (row.availability != 1) {
        //% "Source is offline or unavailable"
        return qtTrId("aegra.backup.source.reason.unavailable");
    }
    if (row.is_read_only) {
        //% "Source is read-only"
        return qtTrId("aegra.backup.source.reason.read_only");
    }
    //% "Source cannot be selected for backup"
    return qtTrId("aegra.backup.source.reason.not_selectable");
}

QVector<SourceInventoryRow> sources_from_variant_list(const QVariantList& items) {
    QVector<SourceInventoryRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const auto map = item.toMap();
        SourceInventoryRow row;
        row.source_id = map.value(QStringLiteral("sourceId")).toString();
        row.display_name = map.value(QStringLiteral("displayName")).toString();
        row.kind = map.value(QStringLiteral("kind")).toLongLong();
        row.availability = map.value(QStringLiteral("availability")).toLongLong();
        row.capacity_bytes = map.value(QStringLiteral("capacityBytes")).toLongLong();
        row.is_system = map.value(QStringLiteral("isSystem")).toBool();
        row.is_read_only = map.value(QStringLiteral("isReadOnly")).toBool();
        row.is_selectable = map.value(QStringLiteral("isSelectable")).toBool();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
