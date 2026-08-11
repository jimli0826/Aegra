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
    /// 1 volume_set, 2 file_set (Service content_kind).
    std::int64_t content_kind{1};
    std::int64_t chain_state{0};
    std::int64_t created_utc_ms{0};
    std::int64_t logical_size_bytes{0};
    std::int64_t stored_size_bytes{0};
    std::int64_t deduplicated_block_count{0};
    std::int64_t deduplicated_logical_bytes{0};
    std::int64_t source_count{0};
    bool has_sidecar{false};
};

// Domain list model for Recovery Points. Owns row data; QML binds to display roles only and does
// not parse JSON, message codes, or raw Service enums for presentation.
class RecoveryPointModel final : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ rowCount NOTIFY countChanged)
    Q_PROPERTY(int volumeSetCount READ volumeSetCount NOTIFY countChanged)
    Q_PROPERTY(int fileSetCount READ fileSetCount NOTIFY countChanged)

  public:
    enum Role : int {
        FileUuidRole = Qt::UserRole + 1,
        BackupSetUuidRole,
        ParentUuidRole,
        ParentSummaryTextRole,
        BackupTypeTextRole,
        ContentKindRole,
        ChainCompleteRole,
        ChainStateTextRole,
        ChainDepthRole,
        CreatedUtcMsRole,
        CreatedTextRole,
        LogicalSizeBytesRole,
        LogicalSizeTextRole,
        StoredSizeBytesRole,
        StoredSizeTextRole,
        DeduplicatedBlockCountRole,
        DeduplicatedLogicalBytesRole,
        DeduplicatedLogicalBytesTextRole,
        SourceCountRole,
        HasSidecarRole,
        IsBaselineRole,
    };

    explicit RecoveryPointModel(QObject* parent = nullptr);

    void set_locale_format(LocaleFormat* format);
    void set_rows(QVector<RecoveryPointRow> rows);
    void clear();
    void retranslate();

    [[nodiscard]] int rowCount(const QModelIndex& parent = QModelIndex()) const override;
    [[nodiscard]] QVariant data(const QModelIndex& index, int role) const override;
    [[nodiscard]] QHash<int, QByteArray> roleNames() const override;

    /// Local calendar dates (YYYY-MM-DD) that have at least one recovery point.
    /// Number of recovery points whose content_kind is 1 (volume set).
    [[nodiscard]] int volumeSetCount() const;
    /// Number of recovery points whose content_kind is 2 (file set).
    [[nodiscard]] int fileSetCount() const;
    Q_INVOKABLE [[nodiscard]] QStringList backupDateYmds() const;
    /// Checkpoints for a local date, newest first. Each map: fileUuid, timeText, backupType,
    /// contentKind, sizeText, logicalSizeBytes, sourceCount, createdUtcMs, createdText,
    /// chainComplete, parentSummary, chainDepth, isBaseline.
    Q_INVOKABLE [[nodiscard]] QVariantList checkpointsForDate(const QString& date_ymd) const;
    /// Safe parent summary for details panel: parent time plus short identifier.
    Q_INVOKABLE [[nodiscard]] QVariantMap recoveryPointDetails(const QString& file_uuid) const;

  signals:
    void countChanged();

  private:
    [[nodiscard]] QString backup_type_text(std::int64_t backup_type) const;
    [[nodiscard]] QString chain_state_text(std::int64_t chain_state) const;
    [[nodiscard]] QString parent_summary_text(const RecoveryPointRow& row) const;
    [[nodiscard]] int chain_depth_for(const RecoveryPointRow& row) const;
    [[nodiscard]] const RecoveryPointRow* find_row(const QString& file_uuid) const;
    [[nodiscard]] static QString short_uuid(const QString& uuid);
    [[nodiscard]] static QString local_date_ymd(std::int64_t created_utc_ms);
    [[nodiscard]] static QString local_time_hm(std::int64_t created_utc_ms);

    LocaleFormat* format_{nullptr};
    QVector<RecoveryPointRow> rows_;
};

[[nodiscard]] QVector<RecoveryPointRow> recovery_points_from_variant_list(const QVariantList& items);

} // namespace aegra::desktop
