#include "client/models/repository_connection_model.h"

#include <QVariantMap>

#include <utility>

namespace aegra::desktop {
namespace {

constexpr std::int64_t kStateAvailable = 1;
constexpr std::int64_t kStateUnavailable = 2;

} // namespace

RepositoryConnectionModel::RepositoryConnectionModel(QObject* parent)
    : QAbstractListModel(parent) {}

void RepositoryConnectionModel::set_rows(QVector<RepositoryConnectionRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    available_count_ = 0;
    for (const auto& row : rows_) {
        if (is_available_state(row.state)) {
            ++available_count_;
        }
    }
    endResetModel();
    emit countChanged();
}

void RepositoryConnectionModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    available_count_ = 0;
    endResetModel();
    emit countChanged();
}

void RepositoryConnectionModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
}

int RepositoryConnectionModel::availableCount() const noexcept { return available_count_; }

bool RepositoryConnectionModel::contains_available(const QString& connection_id) const {
    for (const auto& row : rows_) {
        if (row.connection_id == connection_id && is_available_state(row.state)) {
            return true;
        }
    }
    return false;
}

std::optional<RepositoryConnectionRow>
RepositoryConnectionModel::find(const QString& connection_id) const {
    for (const auto& row : rows_) {
        if (row.connection_id == connection_id) {
            return row;
        }
    }
    return std::nullopt;
}

QString RepositoryConnectionModel::default_connection_id() const {
    for (const auto& row : rows_) {
        if (row.is_default && is_available_state(row.state)) {
            return row.connection_id;
        }
    }
    for (const auto& row : rows_) {
        if (is_available_state(row.state)) {
            return row.connection_id;
        }
    }
    return {};
}

QString RepositoryConnectionModel::connectionIdAt(const int row) const {
    if (row < 0 || row >= rows_.size()) {
        return {};
    }
    return rows_.at(row).connection_id;
}

int RepositoryConnectionModel::indexOfConnectionId(const QString& connection_id) const {
    if (connection_id.isEmpty()) {
        return -1;
    }
    for (int index = 0; index < rows_.size(); ++index) {
        if (rows_.at(index).connection_id == connection_id) {
            return index;
        }
    }
    return -1;
}

bool RepositoryConnectionModel::isAvailableAt(const int row) const {
    if (row < 0 || row >= rows_.size()) {
        return false;
    }
    return is_available_state(rows_.at(row).state);
}

int RepositoryConnectionModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant RepositoryConnectionModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case ConnectionIdRole:
        return row.connection_id;
    case DisplayNameRole:
        return row.display_name;
    case StateValueRole:
        return static_cast<qint64>(row.state);
    case StateTextRole:
        return state_text(row.state);
    case IsDefaultRole:
        return row.is_default;
    case IsAvailableRole:
        return is_available_state(row.state);
    case CapabilitiesRole:
        return row.capabilities;
    default:
        return {};
    }
}

QHash<int, QByteArray> RepositoryConnectionModel::roleNames() const {
    return {{ConnectionIdRole, "connectionId"},
            {DisplayNameRole, "displayName"},
            {StateValueRole, "stateValue"},
            {StateTextRole, "stateText"},
            {IsDefaultRole, "isDefault"},
            {IsAvailableRole, "isAvailable"},
            {CapabilitiesRole, "capabilities"}};
}

bool RepositoryConnectionModel::is_available_state(const std::int64_t state) noexcept {
    return state == kStateAvailable;
}

QString RepositoryConnectionModel::state_text(const std::int64_t state) const {
    if (state == kStateAvailable) {
        //% "Online"
        return qtTrId("aegra.backup.connection.online");
    }
    if (state == kStateUnavailable) {
        //% "Offline"
        return qtTrId("aegra.backup.connection.offline");
    }
    //% "Unknown"
    return qtTrId("aegra.common.unknown");
}

QVector<RepositoryConnectionRow> connections_from_variant_list(const QVariantList& items) {
    QVector<RepositoryConnectionRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const auto map = item.toMap();
        RepositoryConnectionRow row;
        row.connection_id = map.value(QStringLiteral("connectionId")).toString();
        row.display_name = map.value(QStringLiteral("displayName")).toString();
        row.state = map.value(QStringLiteral("state")).toLongLong();
        row.is_default = map.value(QStringLiteral("isDefault")).toBool();
        row.capabilities = map.value(QStringLiteral("capabilities")).toStringList();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
