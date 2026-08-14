#include "client/models/schedule_list_model.h"

namespace aegra::desktop {

ScheduleListModel::ScheduleListModel(QObject* parent) : QAbstractListModel(parent) {}

void ScheduleListModel::set_items(QVariantList items) {
    beginResetModel();
    rows_.clear();
    rows_.reserve(items.size());
    for (auto& item : items) {
        rows_.push_back(item.toMap());
    }
    endResetModel();
    emit countChanged();
}

void ScheduleListModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    endResetModel();
    emit countChanged();
}

int ScheduleListModel::find_row(const QString& schedule_id) const {
    if (schedule_id.isEmpty()) {
        return -1;
    }
    for (int index = 0; index < rows_.size(); ++index) {
        const auto& row = rows_.at(index);
        if (row.value(QStringLiteral("scheduleId")).toString() == schedule_id ||
            row.value(QStringLiteral("id")).toString() == schedule_id) {
            return index;
        }
    }
    return -1;
}

bool ScheduleListModel::set_enabled(const QString& schedule_id, const bool enabled) {
    return patch_row(schedule_id, {{QStringLiteral("enabled"), enabled}});
}

bool ScheduleListModel::patch_row(const QString& schedule_id, const QVariantMap& fields) {
    const int row = find_row(schedule_id);
    if (row < 0 || fields.isEmpty()) {
        return false;
    }
    bool changed = false;
    QList<int> roles{ModelDataRole};
    for (auto it = fields.constBegin(); it != fields.constEnd(); ++it) {
        if (rows_[row].value(it.key()) == it.value()) {
            continue;
        }
        rows_[row].insert(it.key(), it.value());
        changed = true;
        if (it.key() == QLatin1String("enabled")) {
            roles.push_back(EnabledRole);
        } else if (it.key() == QLatin1String("nextRun")) {
            roles.push_back(NextRunRole);
        }
    }
    if (!changed) {
        return true;
    }
    const auto model_index = index(row, 0);
    emit dataChanged(model_index, model_index, roles);
    return true;
}

bool ScheduleListModel::enabled_for(const QString& schedule_id) const {
    const int row = find_row(schedule_id);
    if (row < 0) {
        return false;
    }
    return rows_.at(row).value(QStringLiteral("enabled")).toBool();
}

QVariantMap ScheduleListModel::item_for(const QString& schedule_id) const {
    const int row = find_row(schedule_id);
    if (row < 0) {
        return {};
    }
    return rows_.at(row);
}

QVariantList ScheduleListModel::items() const {
    QVariantList out;
    out.reserve(rows_.size());
    for (const auto& row : rows_) {
        out.push_back(row);
    }
    return out;
}

int ScheduleListModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant ScheduleListModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case ModelDataRole:
        return row;
    case ScheduleIdRole:
        return row.value(QStringLiteral("scheduleId"));
    case EnabledRole:
        return row.value(QStringLiteral("enabled"));
    case NextRunRole:
        return row.value(QStringLiteral("nextRun"));
    default:
        return {};
    }
}

QHash<int, QByteArray> ScheduleListModel::roleNames() const {
    return {{ModelDataRole, "modelData"},
            {ScheduleIdRole, "scheduleId"},
            {EnabledRole, "enabled"},
            {NextRunRole, "nextRun"}};
}

} // namespace aegra::desktop
