#include "client/models/file_browse_model.h"

#include <QVariantMap>

#include <utility>

namespace aegra::desktop {
namespace {

[[nodiscard]] bool is_selectable_node(const FileBrowseNode& node) noexcept {
    return node.selectability == 1 && node.availability == 1;
}

/// Target-folder picker (singleDirectoryMode) only needs directories; hide files from the tree.
[[nodiscard]] QVector<FileBrowseNode> filter_nodes_for_mode(QVector<FileBrowseNode> nodes,
                                                            const bool single_directory_mode) {
    if (!single_directory_mode) {
        return nodes;
    }
    QVector<FileBrowseNode> directories;
    directories.reserve(nodes.size());
    for (auto& node : nodes) {
        if (node.is_directory) {
            directories.push_back(std::move(node));
        }
    }
    return directories;
}

} // namespace

FileBrowseNode file_browse_node_from_map(const QVariantMap& map, const int depth,
                                         const QString& parent_token) {
    FileBrowseNode node;
    node.node_token = map.value(QStringLiteral("nodeToken")).toString();
    node.parent_token = parent_token;
    node.display_name = map.value(QStringLiteral("displayName")).toString();
    node.entry_kind = map.value(QStringLiteral("entryKind")).toLongLong();
    node.selectability = map.value(QStringLiteral("selectability")).toLongLong();
    node.has_children = map.value(QStringLiteral("hasChildren")).toBool();
    node.is_directory = map.value(QStringLiteral("isDirectory")).toBool();
    node.availability = map.value(QStringLiteral("availability")).toLongLong();
    node.message_code = map.value(QStringLiteral("messageCode")).toString();
    node.depth = depth;
    return node;
}

FileBrowseModel::FileBrowseModel(QObject* parent) : QAbstractListModel(parent) {}

void FileBrowseModel::set_loading(const bool loading) {
    if (loading_ == loading) {
        return;
    }
    loading_ = loading;
    emit loadingChanged();
}

void FileBrowseModel::set_error_text(const QString& text) {
    if (error_text_ == text) {
        return;
    }
    error_text_ = text;
    emit loadingChanged();
}

void FileBrowseModel::clear() {
    if (rows_.isEmpty() && !loading_ && error_text_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    token_index_.clear();
    endResetModel();
    loading_ = false;
    error_text_.clear();
    refresh_selection_cache();
    emit countChanged();
    emit loadingChanged();
}

void FileBrowseModel::set_roots(QVector<FileBrowseNode> roots) {
    auto filtered = filter_nodes_for_mode(std::move(roots), single_directory_mode_);
    beginResetModel();
    rows_ = std::move(filtered);
    token_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        rows_[index].depth = 0;
        rows_[index].parent_token.clear();
        token_index_.insert(rows_[index].node_token, index);
    }
    endResetModel();
    loading_ = false;
    error_text_.clear();
    refresh_selection_cache();
    emit countChanged();
    emit loadingChanged();
}

void FileBrowseModel::set_children(const QString& parent_token, QVector<FileBrowseNode> children) {
    const int parent_index = find_index(parent_token);
    if (parent_index < 0) {
        return;
    }
    auto& parent = rows_[parent_index];
    // Remove existing loaded descendants under this parent.
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
    auto filtered = filter_nodes_for_mode(std::move(children), single_directory_mode_);
    parent.children_loaded = true;
    parent.loading = false;
    parent.expanded = true;
    token_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        token_index_.insert(rows_[index].node_token, index);
    }
    if (!filtered.isEmpty()) {
        const int insert_at = parent_index + 1;
        beginInsertRows(QModelIndex(), insert_at, insert_at + filtered.size() - 1);
        for (int offset = 0; offset < filtered.size(); ++offset) {
            auto child = std::move(filtered[offset]);
            child.parent_token = parent_token;
            child.depth = parent.depth + 1;
            if (parent.check_state == 2 && is_selectable_node(child) && !single_directory_mode_) {
                child.check_state = 2;
            }
            rows_.insert(insert_at + offset, std::move(child));
        }
        endInsertRows();
    }
    token_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        token_index_.insert(rows_[index].node_token, index);
    }
    emit_row(parent_index);
    refresh_selection_cache();
    emit countChanged();
}

void FileBrowseModel::set_node_loading(const QString& node_token, const bool loading) {
    const int index = find_index(node_token);
    if (index < 0 || rows_[index].loading == loading) {
        return;
    }
    rows_[index].loading = loading;
    emit_row(index);
}

void FileBrowseModel::set_node_expanded(const QString& node_token, const bool expanded) {
    const int index = find_index(node_token);
    if (index < 0 || rows_[index].expanded == expanded) {
        return;
    }
    rows_[index].expanded = expanded;
    if (!expanded) {
        // Hide descendants by collapsing only the flag; keep children for re-expand.
        for (int cursor = index + 1; cursor < rows_.size() && rows_[cursor].depth > rows_[index].depth;
             ++cursor) {
            if (rows_[cursor].depth == rows_[index].depth + 1) {
                rows_[cursor].expanded = false;
            }
        }
    }
    emit_row(index);
    // Visibility is handled via expanded ancestor chain in data() — use filter in QML via depth.
    // For list models without filtering, collapse removes children from view:
    if (!expanded && rows_[index].children_loaded) {
        int remove_start = index + 1;
        int remove_end = remove_start;
        while (remove_end < rows_.size() && rows_[remove_end].depth > rows_[index].depth) {
            ++remove_end;
        }
        if (remove_end > remove_start) {
            // Keep children_loaded false so re-expand re-fetches fresh page.
            beginRemoveRows(QModelIndex(), remove_start, remove_end - 1);
            rows_.remove(remove_start, remove_end - remove_start);
            endRemoveRows();
            rows_[index].children_loaded = false;
            token_index_.clear();
            for (int i = 0; i < rows_.size(); ++i) {
                token_index_.insert(rows_[i].node_token, i);
            }
            emit countChanged();
        }
    }
    emit_row(index);
}

bool FileBrowseModel::loading() const noexcept { return loading_; }

QString FileBrowseModel::errorText() const { return error_text_; }

int FileBrowseModel::selectedCount() const noexcept { return selected_count_; }

QString FileBrowseModel::selectionSummary() const { return selection_summary_; }

bool FileBrowseModel::singleDirectoryMode() const noexcept { return single_directory_mode_; }

void FileBrowseModel::setSingleDirectoryMode(const bool enabled) {
    if (single_directory_mode_ == enabled) {
        return;
    }
    single_directory_mode_ = enabled;
    if (enabled) {
        for (int index = 0; index < rows_.size(); ++index) {
            if (rows_[index].check_state != 0 &&
                (!rows_[index].is_directory || !is_selectable_node(rows_[index]))) {
                rows_[index].check_state = 0;
                emit_row(index);
            }
        }
        // Keep at most one directory checked.
        QString kept;
        for (int index = 0; index < rows_.size(); ++index) {
            if (rows_[index].check_state == 2) {
                if (kept.isEmpty()) {
                    kept = rows_[index].node_token;
                } else {
                    rows_[index].check_state = 0;
                    emit_row(index);
                }
            }
        }
        refresh_selection_cache();
    }
    emit singleDirectoryModeChanged();
}

QVariantList FileBrowseModel::selected_file_selections() const {
    QVariantList selections;
    QHash<QString, bool> fully_checked;
    for (const auto& row : rows_) {
        if (row.check_state == 2) {
            fully_checked.insert(row.node_token, true);
        }
    }
    for (const auto& row : rows_) {
        if (row.check_state != 2 || !is_selectable_node(row)) {
            continue;
        }
        bool ancestor_checked = false;
        QString parent = row.parent_token;
        while (!parent.isEmpty()) {
            if (fully_checked.contains(parent)) {
                ancestor_checked = true;
                break;
            }
            const int parent_index = token_index_.value(parent, -1);
            if (parent_index < 0) {
                break;
            }
            parent = rows_[parent_index].parent_token;
        }
        if (ancestor_checked) {
            continue;
        }
        selections.push_back(QVariantMap{
            {QStringLiteral("nodeToken"), row.node_token},
            {QStringLiteral("recursion"), row.is_directory ? 2 : 1},
            {QStringLiteral("displayLabel"), row.display_name},
        });
        if (selections.size() >= 100) {
            break;
        }
    }
    return selections;
}

QString FileBrowseModel::selected_directory_token() const {
    for (const auto& row : rows_) {
        if (row.check_state == 2 && row.is_directory && is_selectable_node(row)) {
            return row.node_token;
        }
    }
    return {};
}

QString FileBrowseModel::selected_directory_label() const {
    for (const auto& row : rows_) {
        if (row.check_state == 2 && row.is_directory && is_selectable_node(row)) {
            return row.display_name;
        }
    }
    return {};
}

void FileBrowseModel::toggleExpanded(const QString& node_token) {
    const int index = find_index(node_token);
    if (index < 0 || !rows_[index].has_children) {
        return;
    }
    if (rows_[index].expanded) {
        set_node_expanded(node_token, false);
        return;
    }
    if (rows_[index].children_loaded) {
        set_node_expanded(node_token, true);
        return;
    }
    rows_[index].loading = true;
    emit_row(index);
    emit expandRequested(node_token);
}

void FileBrowseModel::toggleChecked(const QString& node_token) {
    const int index = find_index(node_token);
    if (index < 0 || !is_selectable_node(rows_[index])) {
        return;
    }
    if (single_directory_mode_) {
        if (!rows_[index].is_directory) {
            return;
        }
        const bool will_check = rows_[index].check_state != 2;
        for (int cursor = 0; cursor < rows_.size(); ++cursor) {
            const int next = (cursor == index && will_check) ? 2 : 0;
            if (rows_[cursor].check_state != next) {
                rows_[cursor].check_state = next;
                emit_row(cursor);
            }
        }
        refresh_selection_cache();
        return;
    }
    const int next = rows_[index].check_state == 2 ? 0 : 2;
    rows_[index].check_state = next;
    apply_check_to_descendants(index, next);
    recompute_ancestors(index);
    emit_row(index);
    refresh_selection_cache();
}

int FileBrowseModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant FileBrowseModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case NodeTokenRole:
        return row.node_token;
    case DisplayNameRole:
        return row.display_name;
    case EntryKindRole:
        return static_cast<qint64>(row.entry_kind);
    case SelectabilityRole:
        return static_cast<qint64>(row.selectability);
    case HasChildrenRole:
        return row.has_children;
    case IsDirectoryRole:
        return row.is_directory;
    case AvailabilityRole:
        return static_cast<qint64>(row.availability);
    case DepthRole:
        return row.depth;
    case ExpandedRole:
        return row.expanded;
    case LoadingRole:
        return row.loading;
    case CheckStateRole:
        return row.check_state;
    case IsSelectableRole:
        return is_selectable_node(row) && (!single_directory_mode_ || row.is_directory);
    case DisabledReasonRole:
        if (row.selectability == 3) {
            //% "Unsupported"
            return qtTrId("aegra.file.browse.unsupported");
        }
        if (row.availability != 1) {
            //% "Unavailable"
            return qtTrId("aegra.file.browse.unavailable");
        }
        if (row.selectability != 1) {
            //% "Not selectable"
            return qtTrId("aegra.file.browse.not_selectable");
        }
        return {};
    default:
        return {};
    }
}

QHash<int, QByteArray> FileBrowseModel::roleNames() const {
    return {{NodeTokenRole, "nodeToken"},
            {DisplayNameRole, "displayName"},
            {EntryKindRole, "entryKind"},
            {SelectabilityRole, "selectability"},
            {HasChildrenRole, "hasChildren"},
            {IsDirectoryRole, "isDirectory"},
            {AvailabilityRole, "availability"},
            {DepthRole, "depth"},
            {ExpandedRole, "expanded"},
            {LoadingRole, "nodeLoading"},
            {CheckStateRole, "checkState"},
            {IsSelectableRole, "isSelectable"},
            {DisabledReasonRole, "disabledReason"}};
}

int FileBrowseModel::find_index(const QString& node_token) const {
    return token_index_.value(node_token, -1);
}

void FileBrowseModel::recompute_ancestors(const int index) {
    QString parent = rows_[index].parent_token;
    while (!parent.isEmpty()) {
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
            if (!is_selectable_node(rows_[cursor])) {
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
        parent = rows_[parent_index].parent_token;
    }
}

void FileBrowseModel::apply_check_to_descendants(const int index, const int check_state) {
    const int base_depth = rows_[index].depth;
    for (int cursor = index + 1; cursor < rows_.size() && rows_[cursor].depth > base_depth;
         ++cursor) {
        if (!is_selectable_node(rows_[cursor])) {
            continue;
        }
        if (rows_[cursor].check_state != check_state) {
            rows_[cursor].check_state = check_state;
            emit_row(cursor);
        }
    }
}

void FileBrowseModel::emit_row(const int index) {
    if (index < 0 || index >= rows_.size()) {
        return;
    }
    const auto model_index = this->index(index, 0);
    emit dataChanged(model_index, model_index);
}

void FileBrowseModel::refresh_selection_cache() {
    selected_count_ = 0;
    QStringList labels;
    for (const auto& row : rows_) {
        if (row.check_state != 2 || !is_selectable_node(row)) {
            continue;
        }
        bool ancestor_checked = false;
        QString parent = row.parent_token;
        while (!parent.isEmpty()) {
            const int parent_index = token_index_.value(parent, -1);
            if (parent_index < 0) {
                break;
            }
            if (rows_[parent_index].check_state == 2) {
                ancestor_checked = true;
                break;
            }
            parent = rows_[parent_index].parent_token;
        }
        if (ancestor_checked) {
            continue;
        }
        ++selected_count_;
        if (labels.size() < 3) {
            labels.push_back(row.display_name);
        }
    }
    if (selected_count_ == 0) {
        selection_summary_.clear();
    } else if (selected_count_ <= labels.size()) {
        selection_summary_ = labels.join(QStringLiteral(", "));
    } else {
        selection_summary_ =
            //% "%1 (+%2 more)"
            qtTrId("aegra.file.browse.selection_more")
                .arg(labels.join(QStringLiteral(", ")))
                .arg(selected_count_ - labels.size());
    }
    emit selectionChanged();
}

} // namespace aegra::desktop
