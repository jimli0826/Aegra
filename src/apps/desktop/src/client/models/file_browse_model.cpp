#include "client/models/file_browse_model.h"

#include <QStringList>
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
    locked_hydrate_in_progress_ = false;
    error_text_.clear();
    refresh_selection_cache();
    emit countChanged();
    emit loadingChanged();
}

void FileBrowseModel::set_roots(QVector<FileBrowseNode> roots) {
    locked_hydrate_in_progress_ = false;
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
    apply_locked_selection();
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
    // Locked hydrate keeps the parent collapsed; QML hides non-visible rows.
    parent.expanded = !locked_hydrate_in_progress_;
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
    notify_row_visibility();
    apply_locked_selection();
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
        for (int cursor = index + 1; cursor < rows_.size() && rows_[cursor].depth > rows_[index].depth;
             ++cursor) {
            if (rows_[cursor].depth == rows_[index].depth + 1) {
                rows_[cursor].expanded = false;
            }
        }
    }
    emit_row(index);
    // User collapse removes loaded children so the next expand re-fetches.
    if (!expanded && rows_[index].children_loaded) {
        int remove_start = index + 1;
        int remove_end = remove_start;
        while (remove_end < rows_.size() && rows_[remove_end].depth > rows_[index].depth) {
            ++remove_end;
        }
        if (remove_end > remove_start) {
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
    notify_row_visibility();
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

void FileBrowseModel::setSelectionLocked(const bool locked) {
    if (selection_locked_ == locked) {
        return;
    }
    selection_locked_ = locked;
    emit selectionLockedChanged();
    if (locked) {
        apply_locked_selection();
    }
}

bool FileBrowseModel::selectionLocked() const noexcept { return selection_locked_; }

void FileBrowseModel::setLockedDisplayChains(const QVariantList& chains) {
    locked_chains_.clear();
    locked_chains_.reserve(static_cast<int>(chains.size()));
    for (const auto& item : chains) {
        QStringList names;
        if (item.canConvert<QStringList>()) {
            names = item.toStringList();
        } else if (item.canConvert<QVariantList>()) {
            for (const auto& part : item.toList()) {
                const auto name = part.toString();
                if (!name.isEmpty()) {
                    names.push_back(name);
                }
            }
        }
        if (!names.isEmpty()) {
            locked_chains_.push_back(std::move(names));
        }
    }
    apply_locked_selection();
}

void FileBrowseModel::clearChecks() {
    bool changed = false;
    for (int index = 0; index < rows_.size(); ++index) {
        if (rows_[index].check_state == 0) {
            continue;
        }
        rows_[index].check_state = 0;
        emit_row(index);
        changed = true;
    }
    if (changed) {
        refresh_selection_cache();
    }
}

void FileBrowseModel::toggleChecked(const QString& node_token) {
    if (selection_locked_) {
        return;
    }
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
    case RowVisibleRole:
        return is_row_visible(index.row());
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
            {DisabledReasonRole, "disabledReason"},
            {RowVisibleRole, "rowVisible"}};
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

QStringList FileBrowseModel::display_path(const int index) const {
    QStringList names;
    if (index < 0 || index >= rows_.size()) {
        return names;
    }
    QString token = rows_[index].node_token;
    while (!token.isEmpty()) {
        const int cursor = find_index(token);
        if (cursor < 0) {
            break;
        }
        names.prepend(rows_[cursor].display_name);
        token = rows_[cursor].parent_token;
    }
    return names;
}

namespace {

[[nodiscard]] bool names_equal(const QStringList& left, const int left_offset,
                               const QStringList& right) noexcept {
    if (left_offset < 0 || left.size() - left_offset != right.size()) {
        return false;
    }
    for (int index = 0; index < right.size(); ++index) {
        if (left.at(left_offset + index) != right.at(index)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool names_are_prefix(const QStringList& left, const int left_offset,
                                    const QStringList& right) noexcept {
    if (left_offset < 0 || left.size() - left_offset >= right.size()) {
        return false;
    }
    for (int index = 0; index < left.size() - left_offset; ++index) {
        if (left.at(left_offset + index) != right.at(index)) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool is_special_folder_root_name(const QString& name) noexcept {
    return name == QLatin1String("Desktop") || name == QLatin1String("Downloads") ||
           name == QLatin1String("Documents") || name == QLatin1String("Pictures") ||
           name == QLatin1String("Music") || name == QLatin1String("Videos");
}

[[nodiscard]] bool is_complete_locked_match(const QStringList& names,
                                            const QStringList& chain) noexcept {
    return names_equal(names, 0, chain) || names_equal(names, 1, chain);
}

[[nodiscard]] bool is_locked_prefix(const QStringList& names, const QStringList& chain) noexcept {
    return names_are_prefix(names, 0, chain) || names_are_prefix(names, 1, chain);
}

} // namespace

bool FileBrowseModel::is_row_visible(const int index) const {
    if (index < 0 || index >= rows_.size()) {
        return false;
    }
    QString parent = rows_[index].parent_token;
    while (!parent.isEmpty()) {
        const int parent_index = find_index(parent);
        if (parent_index < 0) {
            return false;
        }
        if (!rows_[parent_index].expanded) {
            return false;
        }
        parent = rows_[parent_index].parent_token;
    }
    return true;
}

void FileBrowseModel::notify_row_visibility() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(this->index(0), this->index(rows_.size() - 1),
                     {static_cast<int>(ExpandedRole), static_cast<int>(RowVisibleRole)});
}

void FileBrowseModel::remove_descendants(const int parent_index) {
    if (parent_index < 0 || parent_index >= rows_.size()) {
        return;
    }
    int remove_start = parent_index + 1;
    int remove_end = remove_start;
    while (remove_end < rows_.size() && rows_[remove_end].depth > rows_[parent_index].depth) {
        ++remove_end;
    }
    if (remove_end <= remove_start) {
        return;
    }
    beginRemoveRows(QModelIndex(), remove_start, remove_end - 1);
    rows_.remove(remove_start, remove_end - remove_start);
    endRemoveRows();
    token_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        token_index_.insert(rows_[index].node_token, index);
    }
    emit countChanged();
}

void FileBrowseModel::prune_locked_hydrate_dead_ends(const QVector<bool>& chain_resolved) {
    if (!locked_hydrate_in_progress_ || chain_resolved.size() != locked_chains_.size()) {
        return;
    }
    // Only drop dig branches that cannot help remaining unresolved chains. When every chain is
    // already matched, leave children for collapse_locked_view (do not empty the tree first).
    bool any_unresolved = false;
    for (const bool resolved : chain_resolved) {
        if (!resolved) {
            any_unresolved = true;
            break;
        }
    }
    if (!any_unresolved) {
        return;
    }
    for (int index = rows_.size() - 1; index >= 0; --index) {
        if (rows_[index].depth != 0 || !rows_[index].children_loaded) {
            continue;
        }
        bool useful = false;
        for (int child = index + 1;
             child < rows_.size() && rows_[child].depth > 0 && !useful; ++child) {
            const auto names = display_path(child);
            for (int chain_index = 0; chain_index < locked_chains_.size(); ++chain_index) {
                if (chain_resolved[chain_index]) {
                    continue;
                }
                if (is_complete_locked_match(names, locked_chains_[chain_index]) ||
                    is_locked_prefix(names, locked_chains_[chain_index])) {
                    useful = true;
                    break;
                }
            }
        }
        if (useful) {
            continue;
        }
        // Keep children_loaded so this root is not re-expanded this hydrate pass.
        rows_[index].expanded = false;
        remove_descendants(index);
    }
}

void FileBrowseModel::collapse_locked_view() {
    // Drop hydrated children from the model so ListView does not keep empty gaps.
    // Preserve root check_state (full / partial). User expand will re-fetch children.
    // Service tokens for dropped nodes are released on the next root browse.
    bool has_deep = false;
    for (const auto& row : rows_) {
        if (row.depth != 0) {
            has_deep = true;
            break;
        }
    }
    if (!has_deep) {
        // Never move roots out of rows_ on this path — a prior prune may have already removed
        // descendants; moved-from QString members would blank every SOURCE label in QML.
        for (int index = 0; index < rows_.size(); ++index) {
            auto& row = rows_[index];
            if (!row.expanded && !row.children_loaded && !row.loading) {
                continue;
            }
            row.expanded = false;
            row.children_loaded = false;
            row.loading = false;
            emit_row(index);
        }
        notify_row_visibility();
        return;
    }
    QVector<FileBrowseNode> roots;
    roots.reserve(rows_.size());
    for (auto& row : rows_) {
        if (row.depth != 0) {
            continue;
        }
        row.expanded = false;
        row.loading = false;
        row.children_loaded = false;
        roots.push_back(std::move(row));
    }
    beginResetModel();
    rows_ = std::move(roots);
    token_index_.clear();
    for (int index = 0; index < rows_.size(); ++index) {
        token_index_.insert(rows_[index].node_token, index);
    }
    endResetModel();
    emit countChanged();
    refresh_selection_cache();
    notify_row_visibility();
}

void FileBrowseModel::apply_locked_selection() {
    if (!selection_locked_ || rows_.isEmpty()) {
        return;
    }
    // User expand sets parent.expanded before set_children → apply_locked_selection. Keep the
    // browsable tree open: refresh checks on loaded rows only; do not restart dig-and-collapse
    // (that path used to blank root display names via collapse_locked_view).
    bool user_tree_open = false;
    if (!locked_hydrate_in_progress_) {
        for (const auto& row : rows_) {
            if (row.depth == 0 && row.expanded) {
                user_tree_open = true;
                break;
            }
        }
    }
    if (!user_tree_open) {
        for (int index = 0; index < rows_.size(); ++index) {
            if (rows_[index].check_state == 0) {
                continue;
            }
            rows_[index].check_state = 0;
            emit_row(index);
        }
    }
    if (locked_chains_.isEmpty()) {
        refresh_selection_cache();
        return;
    }
    QVector<bool> chain_resolved(locked_chains_.size(), false);
    for (int index = 0; index < rows_.size(); ++index) {
        auto& row = rows_[index];
        const auto names = display_path(index);
        for (int chain_index = 0; chain_index < locked_chains_.size(); ++chain_index) {
            if (!is_complete_locked_match(names, locked_chains_[chain_index])) {
                continue;
            }
            chain_resolved[chain_index] = true;
            if (is_selectable_node(row) && row.check_state != 2) {
                row.check_state = 2;
                apply_check_to_descendants(index, 2);
                emit_row(index);
                recompute_ancestors(index);
            }
        }
    }
    if (user_tree_open) {
        refresh_selection_cache();
        notify_row_visibility();
        return;
    }
    prune_locked_hydrate_dead_ends(chain_resolved);
    QString expand_token;
    for (int index = 0; index < rows_.size() && expand_token.isEmpty(); ++index) {
        const auto& row = rows_[index];
        if (!row.is_directory || !row.has_children || row.children_loaded || row.loading) {
            continue;
        }
        const auto names = display_path(index);
        for (int chain_index = 0; chain_index < locked_chains_.size(); ++chain_index) {
            if (chain_resolved[chain_index]) {
                continue;
            }
            // Special-folder roots (Downloads, …) are never dug for volume-relative chains —
            // expanding them lists the folder contents and confuses the locked edit tree.
            // They only match as complete roots via short product labels.
            if (row.depth == 0 && is_special_folder_root_name(row.display_name)) {
                continue;
            }
            // Volume-relative chains do not include the volume root label, so dig under volumes.
            if (row.depth == 0 || is_locked_prefix(names, locked_chains_[chain_index])) {
                expand_token = row.node_token;
                break;
            }
        }
    }
    refresh_selection_cache();
    if (!expand_token.isEmpty()) {
        locked_hydrate_in_progress_ = true;
        emit expandRequested(expand_token);
        return;
    }
    const bool finishing_hydrate = locked_hydrate_in_progress_;
    locked_hydrate_in_progress_ = false;
    if (finishing_hydrate) {
        collapse_locked_view();
    } else {
        notify_row_visibility();
    }
}

} // namespace aegra::desktop
