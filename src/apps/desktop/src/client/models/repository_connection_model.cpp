#include "client/models/repository_connection_model.h"

#include "locale/message_code_map.h"

#include <QHash>
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
    // Keep session probe errors and free-space probes across Service snapshot reloads.
    QHash<QString, QString> probe_error_codes;
    QHash<QString, qint64> free_bytes_by_id;
    QHash<QString, bool> refreshing_by_id;
    for (const auto& existing : rows_) {
        if (!existing.probe_error_code.isEmpty()) {
            probe_error_codes.insert(existing.connection_id, existing.probe_error_code);
        }
        if (existing.free_bytes) {
            free_bytes_by_id.insert(existing.connection_id, *existing.free_bytes);
        }
        if (existing.refreshing) {
            refreshing_by_id.insert(existing.connection_id, true);
        }
    }
    beginResetModel();
    rows_ = std::move(rows);
    available_count_ = 0;
    for (auto& row : rows_) {
        if (is_available_state(row.state)) {
            ++available_count_;
            row.probe_error_code.clear();
            if (free_bytes_by_id.contains(row.connection_id)) {
                row.free_bytes = free_bytes_by_id.value(row.connection_id);
            }
        } else {
            row.probe_error_code = probe_error_codes.value(row.connection_id);
            row.free_bytes = std::nullopt;
        }
        row.refreshing = refreshing_by_id.value(row.connection_id, false);
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

QStringList RepositoryConnectionModel::connection_ids() const {
    QStringList result;
    result.reserve(rows_.size());
    for (const auto& row : rows_) {
        result.push_back(row.connection_id);
    }
    return result;
}

bool RepositoryConnectionModel::set_refreshing(const QString& connection_id,
                                                const bool refreshing) {
    const auto row_index = indexOfConnectionId(connection_id);
    if (row_index < 0 || rows_[row_index].refreshing == refreshing) {
        return false;
    }
    rows_[row_index].refreshing = refreshing;
    const auto changed = index(row_index, 0);
    emit dataChanged(changed, changed, {StateTextRole, IsRefreshingRole});
    return true;
}

void RepositoryConnectionModel::clear_refreshing() {
    for (int row_index = 0; row_index < rows_.size(); ++row_index) {
        if (!rows_[row_index].refreshing) {
            continue;
        }
        rows_[row_index].refreshing = false;
        const auto changed = index(row_index, 0);
        emit dataChanged(changed, changed, {StateTextRole, IsRefreshingRole});
    }
}

bool RepositoryConnectionModel::set_probe_error(const QString& connection_id,
                                                const QString& message_code) {
    const auto row_index = indexOfConnectionId(connection_id);
    if (row_index < 0 || rows_[row_index].probe_error_code == message_code) {
        return false;
    }
    rows_[row_index].probe_error_code = message_code;
    const auto changed = index(row_index, 0);
    emit dataChanged(changed, changed, {ProbeErrorTextRole});
    return true;
}

void RepositoryConnectionModel::clear_probe_error(const QString& connection_id) {
    const auto row_index = indexOfConnectionId(connection_id);
    if (row_index < 0 || rows_[row_index].probe_error_code.isEmpty()) {
        return;
    }
    rows_[row_index].probe_error_code.clear();
    const auto changed = index(row_index, 0);
    emit dataChanged(changed, changed, {ProbeErrorTextRole});
}

bool RepositoryConnectionModel::set_free_bytes(const QString& connection_id,
                                               const std::optional<qint64> free_bytes) {
    const auto row_index = indexOfConnectionId(connection_id);
    if (row_index < 0 || rows_[row_index].free_bytes == free_bytes) {
        return false;
    }
    rows_[row_index].free_bytes = free_bytes;
    const auto changed = index(row_index, 0);
    emit dataChanged(changed, changed, {FreeBytesRole, HasFreeBytesRole});
    return true;
}

std::optional<qint64>
RepositoryConnectionModel::free_bytes_for_locator(const QString& locator) const {
    if (locator.isEmpty()) {
        return std::nullopt;
    }
    for (const auto& row : rows_) {
        if (row.locator == locator && row.free_bytes) {
            return row.free_bytes;
        }
    }
    return std::nullopt;
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
    case LocatorRole:
        return row.locator;
    case StateValueRole:
        return static_cast<qint64>(row.state);
    case StateTextRole:
        return state_text(row);
    case IsDefaultRole:
        return row.is_default;
    case IsAvailableRole:
        return is_available_state(row.state);
    case IsRefreshingRole:
        return row.refreshing;
    case ProbeErrorTextRole:
        return row.probe_error_code.isEmpty() ? QString{}
                                              : localize_message_code(row.probe_error_code);
    case FreeBytesRole:
        return row.free_bytes ? QVariant{*row.free_bytes} : QVariant{};
    case HasFreeBytesRole:
        return row.free_bytes.has_value();
    case CapabilitiesRole:
        return row.capabilities;
    default:
        return {};
    }
}

QHash<int, QByteArray> RepositoryConnectionModel::roleNames() const {
    return {{ConnectionIdRole, "connectionId"},
            {DisplayNameRole, "displayName"},
            {LocatorRole, "locator"},
            {StateValueRole, "stateValue"},
            {StateTextRole, "stateText"},
            {IsDefaultRole, "isDefault"},
            {IsAvailableRole, "isAvailable"},
            {IsRefreshingRole, "isRefreshing"},
            {ProbeErrorTextRole, "probeErrorText"},
            {FreeBytesRole, "freeBytes"},
            {HasFreeBytesRole, "hasFreeBytes"},
            {CapabilitiesRole, "capabilities"}};
}

bool RepositoryConnectionModel::is_available_state(const std::int64_t state) noexcept {
    return state == kStateAvailable;
}

QString RepositoryConnectionModel::state_text(const RepositoryConnectionRow& row) const {
    if (row.refreshing) {
        //% "Loading"
        return qtTrId("aegra.common.loading");
    }
    if (row.state == kStateAvailable) {
        //% "Online"
        return qtTrId("aegra.backup.connection.online");
    }
    if (row.state == kStateUnavailable) {
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
        row.locator = map.value(QStringLiteral("locator")).toString();
        row.state = map.value(QStringLiteral("state")).toLongLong();
        row.is_default = map.value(QStringLiteral("isDefault")).toBool();
        row.capabilities = map.value(QStringLiteral("capabilities")).toStringList();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
