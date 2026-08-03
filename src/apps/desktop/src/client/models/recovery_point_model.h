#pragma once

#include <QAbstractListModel>
#include <QString>
#include <QVariantList>
#include <QVector>

#include <cstdint>

namespace aegra::desktop {

class LocaleFormat;

struct RecoveryPointRow final {
    QString file_uuid;
    QString backup_set_uuid;
    QString parent_uuid;
    std::int64_t backup_type{0};
    std::int64_t chain_state{0};
    std::int64_t created_utc_ms{0};
    std::int64_t logical_size_bytes{0};
    std::int64_t stored_size_bytes{0};
    std::int64_t source_count{0};
    bool has_sidecar{false};
};

// Domain list model for Recovery Points. Owns row data; QML binds to display roles only and does
// not parse JSON, message codes, or raw Service enums for presentation.
class RecoveryPointModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)

  public:
    enum Role : int {
        FileUuidRole = Qt::UserRole + 1,
        BackupSetUuidRole,
        ParentUuidRole,
        BackupTypeTextRole,
        ChainCompleteRole,
        ChainStateTextRole,
        CreatedUtcMsRole,
        CreatedTextRole,
        LogicalSizeBytesRole,
        LogicalSizeTextRole,
        StoredSizeBytesRole,
        StoredSizeTextRole,
        SourceCountRole,
        HasSidecarRole,
    };

    explicit RecoveryPointModel(QObject* parent = nullptr);

    void set_locale_format(LocaleFormat* format);
    void set_rows(QVector<RecoveryPointRow> rows);
    void clear();
    void retranslate();

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

  signals:
    void countChanged();

  private:
    [[nodiscard]] QString backup_type_text(std::int64_t backup_type) const;
    [[nodiscard]] QString chain_state_text(std::int64_t chain_state) const;

    LocaleFormat* format_{nullptr};
    QVector<RecoveryPointRow> rows_;
};

[[nodiscard]] QVector<RecoveryPointRow> recovery_points_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
