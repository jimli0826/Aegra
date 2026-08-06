#include "client/models/source_inventory_model.h"

#include "locale/locale_format.h"

#include <QHash>
#include <QVariantMap>

#include <algorithm>
#include <utility>

namespace aegra::desktop {

SourceInventoryModel::SourceInventoryModel(QObject* parent) : QAbstractListModel(parent) {}

void SourceInventoryModel::set_locale_format(LocaleFormat* format) { format_ = format; }

void SourceInventoryModel::set_rows(QVector<SourceInventoryRow> rows) {
    beginResetModel();
    rows_ = std::move(rows);
    selectable_count_ = 0;
    for (const auto& row : rows_) {
        if (row.is_selectable) {
            ++selectable_count_;
        }
    }
    endResetModel();
    emit countChanged();
}

void SourceInventoryModel::clear() {
    if (rows_.isEmpty()) {
        return;
    }
    beginResetModel();
    rows_.clear();
    selectable_count_ = 0;
    endResetModel();
    emit countChanged();
}

void SourceInventoryModel::retranslate() {
    if (rows_.isEmpty()) {
        return;
    }
    emit dataChanged(index(0, 0), index(rows_.size() - 1, 0));
}

int SourceInventoryModel::selectableCount() const noexcept { return selectable_count_; }

bool SourceInventoryModel::contains_selectable(const QString& source_id) const {
    for (const auto& row : rows_) {
        if (row.source_id == source_id && row.is_selectable) {
            return true;
        }
    }
    return false;
}

std::optional<SourceInventoryRow> SourceInventoryModel::find(const QString& source_id) const {
    for (const auto& row : rows_) {
        if (row.source_id == source_id) {
            return row;
        }
    }
    return std::nullopt;
}

int SourceInventoryModel::rowCount(const QModelIndex& parent) const {
    return parent.isValid() ? 0 : rows_.size();
}

QVariant SourceInventoryModel::data(const QModelIndex& index, const int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= rows_.size()) {
        return {};
    }
    const auto& row = rows_.at(index.row());
    switch (role) {
    case SourceIdRole:
        return row.source_id;
    case DisplayNameRole:
        return row.display_name;
    case CapacityBytesRole:
        return static_cast<qint64>(row.capacity_bytes);
    case CapacityTextRole:
        return format_ != nullptr ? format_->format_bytes(row.capacity_bytes) : QString{};
    case AvailabilityTextRole:
        return availability_text(row);
    case IsSystemRole:
        return row.is_system;
    case IsReadOnlyRole:
        return row.is_read_only;
    case IsSelectableRole:
        return row.is_selectable;
    case DisabledReasonTextRole:
        return disabled_reason_text(row);
    case DiskNumberRole:
        return static_cast<int>(row.disk_number);
    case MountLetterRole:
        return row.mount_letter;
    case VolumeLabelRole:
        return row.volume_label;
    case HealthStatusRole:
        return row.health_status;
    case PartitionStyleRole:
        return row.partition_style;
    default:
        return {};
    }
}

QHash<int, QByteArray> SourceInventoryModel::roleNames() const {
    return {{SourceIdRole, "sourceId"},
            {DisplayNameRole, "displayName"},
            {CapacityBytesRole, "capacityBytes"},
            {CapacityTextRole, "capacityText"},
            {AvailabilityTextRole, "availabilityText"},
            {IsSystemRole, "isSystem"},
            {IsReadOnlyRole, "isReadOnly"},
            {IsSelectableRole, "isSelectable"},
            {DisabledReasonTextRole, "disabledReasonText"},
            {DiskNumberRole, "diskNumber"},
            {MountLetterRole, "mountLetter"},
            {VolumeLabelRole, "volumeLabel"},
            {HealthStatusRole, "healthStatus"},
            {PartitionStyleRole, "partitionStyle"}};
}

QVariantList SourceInventoryModel::disksTree() const {
    // Group volumes by disk_number for Backup wizard (old disksTree).
    // Service emits disk.* shells for every PhysicalDrive (empty disks + restore target ids).
    struct DiskAcc {
        std::uint32_t number{0};
        bool is_system{false};
        QString partition_style{QStringLiteral("GPT")};
        QString media_type{QStringLiteral("Unknown")};
        std::uint64_t capacity_bytes{0};
        std::uint64_t volume_capacity_bytes{0};
        // Used = sum(volume total − volume free), matching old HomeBackend.
        std::uint64_t used_bytes{0};
        QVariantList volumes;
    };
    QVector<DiskAcc> disks;
    QHash<std::uint32_t, int> index_by_disk;

    const auto ensure_disk = [&](const SourceInventoryRow& row) -> DiskAcc& {
        auto it = index_by_disk.find(row.disk_number);
        if (it == index_by_disk.end()) {
            index_by_disk.insert(row.disk_number, disks.size());
            DiskAcc acc;
            acc.number = row.disk_number;
            acc.is_system = row.is_system;
            acc.partition_style =
                row.partition_style.isEmpty() ? QStringLiteral("GPT") : row.partition_style;
            acc.media_type =
                row.media_type.isEmpty() ? QStringLiteral("Unknown") : row.media_type;
            disks.push_back(std::move(acc));
            it = index_by_disk.find(row.disk_number);
        }
        auto& disk = disks[*it];
        if (row.is_system) {
            disk.is_system = true;
        }
        if (row.disk_capacity_bytes > 0) {
            disk.capacity_bytes =
                (std::max)(disk.capacity_bytes, static_cast<std::uint64_t>(row.disk_capacity_bytes));
        }
        if (!row.partition_style.isEmpty()) {
            disk.partition_style = row.partition_style;
        }
        if (!row.media_type.isEmpty() && row.media_type != QStringLiteral("Unknown")) {
            disk.media_type = row.media_type;
        }
        return disk;
    };

    for (const auto& row : rows_) {
        auto& disk = ensure_disk(row);
        // disk.N shells are physical-drive placeholders, not backup volumes.
        if (row.source_id.startsWith(QStringLiteral("disk."))) {
            continue;
        }
        const auto volume_capacity = static_cast<std::uint64_t>(row.capacity_bytes);
        const auto volume_free =
            static_cast<std::uint64_t>((std::max)(std::int64_t{0}, row.free_bytes));
        const auto volume_used =
            volume_capacity > volume_free ? volume_capacity - volume_free : 0ULL;
        disk.volume_capacity_bytes += volume_capacity;
        disk.used_bytes += volume_used;
        const QString size_text =
            format_ != nullptr ? format_->format_bytes(row.capacity_bytes) : QString{};
        const QString label =
            !row.volume_label.isEmpty()
                ? row.volume_label
                : (!row.display_name.isEmpty() ? row.display_name : row.mount_letter);
        disk.volumes.push_back(QVariantMap{
            {QStringLiteral("sourceId"), row.source_id},
            {QStringLiteral("name"), label},
            {QStringLiteral("letter"), row.mount_letter},
            {QStringLiteral("capacityBytes"), static_cast<qint64>(row.capacity_bytes)},
            {QStringLiteral("freeBytes"), static_cast<qint64>(row.free_bytes)},
            {QStringLiteral("size"), size_text},
            {QStringLiteral("status"),
             row.health_status.isEmpty() ? QStringLiteral("Healthy") : row.health_status},
            {QStringLiteral("selectable"), row.is_selectable},
            {QStringLiteral("isSystem"), row.is_system},
        });
    }

    std::sort(disks.begin(), disks.end(),
              [](const DiskAcc& left, const DiskAcc& right) { return left.number < right.number; });

    QVariantList out;
    out.reserve(disks.size());
    for (const auto& disk : disks) {
        const auto capacity_bytes = (std::max)(disk.capacity_bytes, disk.volume_capacity_bytes);
        // Old Home: free = disk_total − sum(vol_total − vol_free).
        const auto used_bytes = (std::min)(disk.used_bytes, capacity_bytes);
        const auto free_bytes = capacity_bytes > used_bytes ? capacity_bytes - used_bytes : 0ULL;
        const double percent_used =
            capacity_bytes > 0
                ? static_cast<double>(used_bytes) / static_cast<double>(capacity_bytes)
                : 0.0;
        const QString size_text =
            format_ != nullptr ? format_->format_bytes(static_cast<std::int64_t>(capacity_bytes))
                               : QString{};
        const QString free_text =
            format_ != nullptr ? format_->format_bytes(static_cast<std::int64_t>(free_bytes))
                               : QString{};
        const bool unallocated = disk.volumes.isEmpty();
        const bool selectable = std::ranges::any_of(disk.volumes, [](const QVariant& volume) {
            return volume.toMap().value(QStringLiteral("selectable")).toBool();
        });
        QString type = disk.partition_style;
        if (unallocated && !type.isEmpty()) {
            type += QStringLiteral(" · Unallocated");
        } else if (unallocated) {
            type = QStringLiteral("Unallocated");
        }
        out.push_back(QVariantMap{
            {QStringLiteral("diskNumber"), static_cast<int>(disk.number)},
            {QStringLiteral("name"),
             QStringLiteral("Disk %1").arg(static_cast<int>(disk.number))},
            {QStringLiteral("capacityBytes"), static_cast<qint64>(capacity_bytes)},
            {QStringLiteral("size"), size_text},
            {QStringLiteral("freeBytes"), static_cast<qint64>(free_bytes)},
            {QStringLiteral("freeFormatted"), free_text},
            {QStringLiteral("percentUsed"), percent_used},
            {QStringLiteral("type"), type},
            {QStringLiteral("partitionStyle"), disk.partition_style},
            {QStringLiteral("mediaType"), disk.media_type},
            {QStringLiteral("isSystemDisk"), disk.is_system},
            {QStringLiteral("unallocated"), unallocated},
            {QStringLiteral("selectable"), selectable},
            {QStringLiteral("volumes"), disk.volumes},
        });
    }
    return out;
}

QString SourceInventoryModel::availability_text(const SourceInventoryRow& row) const {
    if (row.availability == 1) {
        //% "Available"
        return qtTrId("aegra.backup.source.available");
    }
    //% "Unavailable"
    return qtTrId("aegra.backup.source.unavailable");
}

QString SourceInventoryModel::disabled_reason_text(const SourceInventoryRow& row) const {
    if (row.is_selectable) {
        return {};
    }
    if (row.availability != 1) {
        //% "Source is offline or unavailable"
        return qtTrId("aegra.backup.source.reason.unavailable");
    }
    if (row.is_read_only) {
        //% "Source is read-only"
        return qtTrId("aegra.backup.source.reason.read_only");
    }
    //% "Source cannot be selected for backup"
    return qtTrId("aegra.backup.source.reason.not_selectable");
}

QVector<SourceInventoryRow> sources_from_variant_list(const QVariantList& items) {
    QVector<SourceInventoryRow> rows;
    rows.reserve(items.size());
    for (const auto& item : items) {
        const auto map = item.toMap();
        SourceInventoryRow row;
        row.source_id = map.value(QStringLiteral("sourceId")).toString();
        row.display_name = map.value(QStringLiteral("displayName")).toString();
        row.kind = map.value(QStringLiteral("kind")).toLongLong();
        row.availability = map.value(QStringLiteral("availability")).toLongLong();
        row.capacity_bytes = map.value(QStringLiteral("capacityBytes")).toLongLong();
        row.free_bytes = map.value(QStringLiteral("freeBytes")).toLongLong();
        row.disk_capacity_bytes = map.value(QStringLiteral("diskCapacityBytes")).toLongLong();
        row.is_system = map.value(QStringLiteral("isSystem")).toBool();
        row.is_read_only = map.value(QStringLiteral("isReadOnly")).toBool();
        row.is_selectable = map.value(QStringLiteral("isSelectable")).toBool();
        row.disk_number = static_cast<std::uint32_t>(map.value(QStringLiteral("diskNumber")).toUInt());
        row.mount_letter = map.value(QStringLiteral("mountLetter")).toString();
        row.volume_label = map.value(QStringLiteral("volumeLabel")).toString();
        row.health_status = map.value(QStringLiteral("healthStatus")).toString();
        row.partition_style = map.value(QStringLiteral("partitionStyle")).toString();
        row.media_type = map.value(QStringLiteral("mediaType")).toString();
        rows.push_back(std::move(row));
    }
    return rows;
}

} // namespace aegra::desktop
