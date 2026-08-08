#pragma once

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <cstdint>

namespace aegra::desktop {

struct FileRecoverNode final {
    QString entry_id;
    QString parent_entry_id;
    QString display_name;
    std::int64_t entry_kind{2};
    std::uint64_t logical_size_bytes{0};
    bool has_children{false};
    QString message_code;
    int depth{0};
    bool expanded{false};
    bool children_loaded{false};
    bool loading{false};
    int check_state{0};
};

/// Lazy Recovery Point file tree (ListRecoveryPointEntries). Stores entry IDs only.
class FileRecoverModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int selectedCount READ selectedCount NOTIFY selectionChanged)
    Q_PROPERTY(QString selectionSummary READ selectionSummary NOTIFY selectionChanged)
    Q_PROPERTY(bool loading READ loading NOTIFY loadingChanged)
    Q_PROPERTY(QString errorText READ errorText NOTIFY loadingChanged)
    Q_PROPERTY(QString recoveryPointId READ recoveryPointId NOTIFY contextChanged)
    Q_PROPERTY(QString indexGeneration READ indexGeneration NOTIFY contextChanged)

  public:
    enum Role : int {
        EntryIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        EntryKindRole,
        LogicalSizeBytesRole,
        HasChildrenRole,
        DepthRole,
        ExpandedRole,
        LoadingRole,
        CheckStateRole,
        IsDirectoryRole,
    };

    explicit FileRecoverModel(QObject* parent = nullptr);

    void set_context(const QString& recovery_point_id, const QString& connection_id);
    void set_loading(bool loading);
    void set_error_text(const QString& text);
    void clear();
    void set_roots(QVector<FileRecoverNode> roots, const QString& index_generation);
    void set_children(const QString& parent_entry_id, QVector<FileRecoverNode> children,
                      const QString& index_generation);

    [[nodiscard]] bool loading() const noexcept;
    [[nodiscard]] QString errorText() const;
    [[nodiscard]] int selectedCount() const noexcept;
    [[nodiscard]] QString selectionSummary() const;
    [[nodiscard]] QString recoveryPointId() const;
    [[nodiscard]] QString connectionId() const;
    [[nodiscard]] QString indexGeneration() const;
    [[nodiscard]] QStringList selected_entry_ids() const;

    Q_INVOKABLE void toggleExpanded(const QString& entry_id);
    Q_INVOKABLE void toggleChecked(const QString& entry_id);

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();
    void selectionChanged();
    void loadingChanged();
    void contextChanged();
    void expandRequested(const QString& entryId);

  private:
    [[nodiscard]] int find_index(const QString& entry_id) const;
    void recompute_ancestors(int index);
    void apply_check_to_descendants(int index, int check_state);
    void emit_row(int index);
    void refresh_selection_cache();
    void rebuild_index();

    QVector<FileRecoverNode> rows_;
    QHash<QString, int> entry_index_;
    QString recovery_point_id_;
    QString connection_id_;
    QString index_generation_;
    bool loading_{false};
    QString error_text_;
    int selected_count_{0};
    QString selection_summary_;
};

} // namespace aegra::desktop
