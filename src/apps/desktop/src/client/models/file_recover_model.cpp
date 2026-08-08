#include "client/models/file_recover_model.h"

#include <QVariantMap>

#include <utility>

namespace aegra::desktop {

FileRecoverModel::FileRecoverModel(QObject* parent) : QAbstractListModel(parent) {}

void FileRecoverModel::set_context(const QString& recovery_point_id, const QString& connection_id) {
    if (recovery_point_id_ == recovery_point_id && connection_id_ == connection_id) {
        return;
    }
    // clear() also drops context fields — re-apply after so expand can use recoveryPointId.
    clear();
    recovery_point_id_ = recovery_point_id;
    connection_id_ = connection_id;
    emit contextChanged();
}

void FileRecoverModel::set_loading(const bool loading) {
    if (loading_ == loading) {
        return;
    }
    loading_ = loading;
    emit loadingChanged();
}

void FileRecoverModel::set_error_text(const QString& text) {
    if (error_text_ == text) {
        return;
    }
    error_text_ = text;
    emit loadingChanged();
}

void FileRecoverModel::clear() {
    beginResetModel();
    rows_.clear();
    entry_index_.clear();
    index_generation_.clear();
    recovery_point_id_.clear();
    connection_id_.clear();
    endResetModel();
    loading_ = false;
    error_text_.clear();
    refresh_selection_cache();
    emit countChanged();
    emit loadingChanged();
    emit contextChanged();
}

void FileRecoverModel::set_roots(QVector<FileRecoverNode> roots, const QString& index_generation) {
    beginResetModel();
    rows_ = std::move(roots);
    for (auto& row : rows_) {
        row.depth = 0;
        row.parent_entry_id = QStringLiteral("0");
    }
    index_generation_ = index_generation;
    rebuild_index();
    endResetModel();
    loading_ = false;
    error_text_.clear();
    refresh_selection_cache();
    emit countChanged();
    emit loadingChanged();
    emit contextChanged();
}

void FileRecoverModel::set_children(const QString& parent_entry_id,
                                    QVector<FileRecoverNode> children,
                                    const QString& index_generation) {
    if (!index_generation_.isEmpty() && index_generation != index_generation_) {
        set_error_text(QStringLiteral("file_recover.generation_changed"));
        return;
    }
    index_generation_ = index_generation;
    const int parent_index = find_index(parent_entry_id);
    if (parent_index < 0 && parent_entry_id != QStringLiteral("0")) {
        return;
    }
    if (parent_entry_id == QStringLiteral("0")) {
        set_roots(std::move(children), index_generation);
        return;
    }
    auto& parent = rows_[parent_index];
    if (parent.children_loaded) {
        int remove_start = parent_index + 1;
        int remove_end = remove_start;
        while (remove_end < rows_.size() && rows_[remove_end].depth > parent.depth) {
            ++remove_end;
        }
        if (remove_end > remove_start) {
            beginRemoveRows(QModelIndex(), remove_start, remove_end - 1);
            rows_.remove(remove_start, remove_end - remove_start);
            endRemoveRows();
        }
    }
    parent.children_loaded = true;
    parent.loading = false;
    parent.expanded = true;
    if (!children.isEmpty()) {
        const int insert_at = parent_index + 1;
        beginInsertRows(QModelIndex(), insert_at, insert_at + children.size() - 1);
        for (int offset = 0; offset < children.size(); ++offset) {
            auto child = std::move(children[offset]);
            child.parent_entry_id = parent_entry_id;
            child.depth = parent.depth + 1;
            if (parent.check_state == 2) {
                child.check_state = 2;
            }
            rows_.insert(insert_at + offset, std::move(child));
        }
        endInsertRows();
    }
    rebuild_index();
    emit_row(parent_index);
    refresh_selection_cache();
    emit countChanged();
}

bool FileRecoverModel::loading() const noexcept { return loading_; }

QString FileRecoverModel::errorText() const { return error_text_; }

int FileRecoverModel::selectedCount() const noexcept { return selected_count_; }

QString FileRecoverModel::selectionSummary() const { return selection_summary_; }

QString FileRecoverModel::recoveryPointId() const { return recovery_point_id_; }

QString FileRecoverModel::connectionId() const { return connection_id_; }

QString FileRecoverModel::indexGeneration() const { return index_generation_; }

QStringList FileRecoverModel::selected_entry_ids() const {
    QStringList ids;
    QHash<QString, bool> fully_checked;
    for (const auto& row : rows_) {
        if (row.check_state == 2) {
            fully_checked.insert(row.entry_id, true);
        }
    }
    for (const auto& row : rows_) {
        if (row.check_state != 2) {
            continue;
        }
        bool ancestor_checked = false;
        QString parent = row.parent_entry_id;
        while (!parent.isEmpty() && parent != QStringLiteral("0")) {
            if (fully_checked.contains(parent)) {
                ancestor_checked = true;
                break;
            }
            const int parent_index = entry_index_.value(parent, -1);
            if (parent_index < 0) {
                break;
            }
            parent = rows_[parent_index].parent_entry_id;
        }
        if (ancestor_checked) {
            continue;
        }
        ids.push_back(row.entry_id);
        if (ids.size() >= 10'000) {
            break;
        }
    }
    return ids;
}

void FileRecoverModel::toggleExpanded(const QString& entry_id) {
    const int index = find_index(entry_id);
    if (index < 0 || !rows_[index].has_children) {
        return;
    }
    if (rows_[index].expanded) {
        int remove_start = index + 1;
        int remove_end = remove_start;
        while (remove_end < rows_.size() && rows_[remove_end].depth > rows_[index].depth) {
            ++remove_end;
        }
        if (remove_end > remove_start) {
            beginRemoveRows(QModelIndex(), remove_start, remove_end - 1);
            rows_.remove(remove_start, remove_end - remove_start);
            endRemoveRows();
        }
        rows_[index].expanded = false;
        rows_[index].children_loaded = false;
        rebuild_index();
        emit_row(index);
        emit countChanged();
        return;
    }
    if (rows_[index].children_loaded) {
        rows_[index].expanded = true;
        emit_row(index);
        return;
    }
    rows_[index].loading = true;
    emit_row(index);
    emit expandRequested(entry_id);
}

void FileRecoverModel::toggleChecked(const QString& entry_id) {
    const int index = find_index(entry_id);
    if (index < 0) {
        return;
    }
    const int next = rows_[index].check_state == 2 ? 0 : 2;
    rows_[index].check_state = next;
    apply_check_to_descendants(index, next);
    recompute_ancestors(index);
    emit_row(index);
    refresh_selection_cache();
}

int FileRecoverModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant FileRecoverModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case EntryIdRole:
        return row.entry_id;
    case DisplayNameRole:
        return row.display_name;
    case EntryKindRole:
        return static_cast<qint64>(row.entry_kind);
    case LogicalSizeBytesRole:
        return QVariant::fromValue(row.logical_size_bytes);
    case HasChildrenRole:
        return row.has_children;
    case DepthRole:
        return row.depth;
    case ExpandedRole:
        return row.expanded;
    case LoadingRole:
        return row.loading;
    case CheckStateRole:
        return row.check_state;
    case IsDirectoryRole:
        return row.entry_kind == 1 || row.has_children;
    default:
        return {};
    }
}

QHash<int, QByteArray> FileRecoverModel::roleNames() const {
    return {{EntryIdRole, "entryId"},
            {DisplayNameRole, "displayName"},
            {EntryKindRole, "entryKind"},
            {LogicalSizeBytesRole, "logicalSizeBytes"},
            {HasChildrenRole, "hasChildren"},
            {DepthRole, "depth"},
            {ExpandedRole, "expanded"},
            {LoadingRole, "nodeLoading"},
            {CheckStateRole, "checkState"},
            {IsDirectoryRole, "isDirectory"}};
}

int FileRecoverModel::find_index(const QString& entry_id) const {
    return entry_index_.value(entry_id, -1);
}

void FileRecoverModel::recompute_ancestors(const int index) {
    QString parent = rows_[index].parent_entry_id;
    while (!parent.isEmpty() && parent != QStringLiteral("0")) {
        const int parent_index = find_index(parent);
        if (parent_index < 0) {
            break;
        }
        int child_count = 0;
        int checked_count = 0;
        int partial_count = 0;
        for (int cursor = parent_index + 1;
             cursor < rows_.size() && rows_[cursor].depth > rows_[parent_index].depth; ++cursor) {
            if (rows_[cursor].depth != rows_[parent_index].depth + 1) {
                continue;
            }
            ++child_count;
            if (rows_[cursor].check_state == 2) {
                ++checked_count;
            } else if (rows_[cursor].check_state == 1) {
                ++partial_count;
            }
        }
        int next = 0;
        if (child_count > 0 && checked_count == child_count) {
            next = 2;
        } else if (checked_count > 0 || partial_count > 0) {
            next = 1;
        }
        if (rows_[parent_index].check_state != next) {
            rows_[parent_index].check_state = next;
            emit_row(parent_index);
        }
        parent = rows_[parent_index].parent_entry_id;
    }
}

void FileRecoverModel::apply_check_to_descendants(const int index, const int check_state) {
    const int base_depth = rows_[index].depth;
    for (int cursor = index + 1; cursor < rows_.size() && rows_[cursor].depth > base_depth;
         ++cursor) {
        if (rows_[cursor].check_state != check_state) {
            rows_[cursor].check_state = check_state;
            emit_row(cursor);
        }
    }
}

void FileRecoverModel::emit_row(const int index) {
    if (index < 0 || index >= rows_.size()) {
        return;
    }
    const auto model_index = this->index(index, 0);
    emit dataChanged(model_index, model_index);
}

void FileRecoverModel::refresh_selection_cache() {
    const auto ids = selected_entry_ids();
    selected_count_ = ids.size();
    QStringList labels;
    for (const auto& row : rows_) {
        if (!ids.contains(row.entry_id)) {
            continue;
        }
        if (labels.size() < 3) {
            labels.push_back(row.display_name);
        }
    }
    if (selected_count_ == 0) {
        selection_summary_.clear();
    } else if (selected_count_ <= labels.size()) {
        selection_summary_ = labels.join(QStringLiteral(", "));
    } else {
        selection_summary_ = qtTrId("aegra.file.browse.selection_more")
                                 .arg(labels.join(QStringLiteral(", ")))
                                 .arg(selected_count_ - labels.size());
    }
    emit selectionChanged();
}

void FileRecoverModel::rebuild_index() {
    entry_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        entry_index_.insert(rows_[index].entry_id, index);
    }
}

} // namespace aegra::desktop
