#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include <cstdint>
#include <optional>

namespace aegra::desktop {

class LocaleFormat;

struct SourceInventoryRow final {
    QString source_id;
    QString display_name;
    std::int64_t kind{1};
    std::int64_t availability{2};
    std::int64_t capacity_bytes{0};
    std::int64_t free_bytes{0};
    std::int64_t disk_capacity_bytes{0};
    bool is_system{false};
    bool is_read_only{false};
    bool is_selectable{false};
    std::uint32_t disk_number{0};
    std::int64_t offset_bytes{0};
    QString mount_letter;
    QString volume_label;
    QString health_status;
    QString partition_style;
    QString media_type;
};

// Domain list model for Service Inventory sources. QML binds to display roles only.
class SourceInventoryModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int selectableCount READ selectableCount NOTIFY countChanged)
    /// Disk → volumes tree for Backup wizard (old disksTree shape).
    Q_PROPERTY(QVariantList disksTree READ disksTree NOTIFY countChanged)

  public:
    enum Role : int {
        SourceIdRole = Qt::UserRole + 1,
        DisplayNameRole,
        CapacityBytesRole,
        CapacityTextRole,
        AvailabilityTextRole,
        IsSystemRole,
        IsReadOnlyRole,
        IsSelectableRole,
        DisabledReasonTextRole,
        DiskNumberRole,
        MountLetterRole,
        VolumeLabelRole,
        HealthStatusRole,
        PartitionStyleRole,
    };

    explicit SourceInventoryModel(QObject* parent = nullptr);

    void set_locale_format(LocaleFormat* format);
    void set_rows(QVector<SourceInventoryRow> rows);
    void clear();
    void retranslate();

    [[nodiscard]] int selectableCount() const noexcept;
    [[nodiscard]] bool contains_selectable(const QString& source_id) const;
    [[nodiscard]] std::optional<SourceInventoryRow> find(const QString& source_id) const;
    [[nodiscard]] QVariantList disksTree() const;
    /// Maps schedule source_ids onto the current disksTree.
    /// Returns matchCount, volumeKeyList ("dNvM"), expandedDiskList (disk indexes).
    Q_INVOKABLE [[nodiscard]] QVariantMap checkedStateForSourceIds(
        const QVariantList& source_ids) const;

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();

  private:
    [[nodiscard]] QString availability_text(const SourceInventoryRow& row) const;
    [[nodiscard]] QString disabled_reason_text(const SourceInventoryRow& row) const;

    LocaleFormat* format_{nullptr};
    QVector<SourceInventoryRow> rows_;
    int selectable_count_{0};
};

[[nodiscard]] QVector<SourceInventoryRow> sources_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
