#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

namespace aegra::desktop {

/// Schedule table model. Full replace on ListSchedules; single-row dataChanged for enable toggle.
class ScheduleListModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

  public:
    enum Role : int {
        /// Whole row map (keeps BackupPage modelData bindings).
        ModelDataRole = Qt::UserRole + 1,
        ScheduleIdRole,
        EnabledRole,
        NextRunRole,
    };

    explicit ScheduleListModel(QObject* parent = nullptr);

    void set_items(QVariantList items);
    void clear();

    /// Update enabled for one schedule; emits dataChanged only (no model reset).
    [[nodiscard]] bool set_enabled(const QString& schedule_id, bool enabled);
    /// Merge fields into one row (enabled / nextRun / nextRunUtcMs); dataChanged only.
    [[nodiscard]] bool patch_row(const QString& schedule_id, const QVariantMap& fields);

    [[nodiscard]] bool enabled_for(const QString& schedule_id) const;
    [[nodiscard]] QVariantMap item_for(const QString& schedule_id) const;
    [[nodiscard]] QVariantList items() const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();

  private:
    [[nodiscard]] int find_row(const QString& schedule_id) const;

    QVector<QVariantMap> rows_;
};

} // namespace aegra::desktop
