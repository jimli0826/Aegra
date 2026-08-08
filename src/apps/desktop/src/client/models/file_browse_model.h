#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <cstdint>

namespace aegra::desktop {

struct FileBrowseNode final {
    QString node_token;
    QString parent_token;
    QString display_name;
    std::int64_t entry_kind{1};
    std::int64_t selectability{1};
    bool has_children{false};
    bool is_directory{true};
    std::int64_t availability{1};
    QString message_code;
    int depth{0};
    bool expanded{false};
    bool children_loaded{false};
    bool loading{false};
    /// 0 unchecked, 1 partial, 2 checked
    int check_state{0};
};

/// Lazy Service-backed file tree for backup source / restore target browse.
/// Nodes store opaque tokens only — never absolute paths.
class FileBrowseModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionSummary READ selectionSummary NOTIFY selectionChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY loadingChanged)
    /// When true, only one directory may be checked (restore target).
    Q_PROPERTY(bool singleDirectoryMode READ singleDirectoryMode WRITE setSingleDirectoryMode
                   NOTIFY singleDirectoryModeChanged)

  public:
    enum Role : int {
        NodeTokenRole = Qt::UserRole + 1,
        DisplayNameRole,
        EntryKindRole,
        SelectabilityRole,
        HasChildrenRole,
        IsDirectoryRole,
        AvailabilityRole,
        DepthRole,
        ExpandedRole,
        LoadingRole,
        CheckStateRole,
        IsSelectableRole,
        DisabledReasonRole,
    };

    explicit FileBrowseModel(QObject* parent = nullptr);

    void set_loading(bool loading);
    void set_error_text(const QString& text);
    void clear();
    void set_roots(QVector<FileBrowseNode> roots);
    void set_children(const QString& parent_token, QVector<FileBrowseNode> children);
    void set_node_loading(const QString& node_token, bool loading);
    void set_node_expanded(const QString& node_token, bool expanded);

    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] int selectedCount() const noexcept;
    [[nodiscard]] QString selectionSummary() const;
    [[nodiscard]] bool singleDirectoryMode() const noexcept;
    void setSingleDirectoryMode(bool enabled);

    /// Selections for UpsertSchedule file_set (nodeToken, recursion, displayLabel).
    [[nodiscard]] QVariantList selected_file_selections() const;
    /// Single checked directory token (singleDirectoryMode), or empty.
    [[nodiscard]] QString selected_directory_token() const;
    [[nodiscard]] QString selected_directory_label() const;

    Q_INVOKABLE void toggleExpanded(const QString& node_token);
    Q_INVOKABLE void toggleChecked(const QString& node_token);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();
    void selectionChanged();
    void loadingChanged();
    void singleDirectoryModeChanged();
    /// QML/ServiceClient should load children when a node expands and children are not loaded.
    void expandRequested(const QString& nodeToken);

  private:
    [[nodiscard]] int find_index(const QString& node_token) const;
    void recompute_ancestors(int index);
    void apply_check_to_descendants(int index, int check_state);
    void emit_row(int index);
    void refresh_selection_cache();

    QVector<FileBrowseNode> rows_;
    QHash<QString, int> token_index_;
    bool loading_{false};
    bool single_directory_mode_{false};
    QString error_text_;
    int selected_count_{0};
    QString selection_summary_;
};

[[nodiscard]] FileBrowseNode file_browse_node_from_map(const QVariantMap& map, int depth,
                                                      const QString& parent_token);

} // namespace aegra::desktop
