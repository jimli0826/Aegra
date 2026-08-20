#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/locale_format.h"
#include "locale/message_code_map.h"

#include <QHash>
#include <QJsonObject>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <vector>

namespace aegra::desktop {
namespace {

constexpr int kRepositoryQueryDeadlineMilliseconds = 5'000;
constexpr int kRepositoryProbeDeadlineMilliseconds = 35'000;

constexpr qsizetype kMaximumRecoveryPoints = 10'000;

[[nodiscard]] bool is_unc_path(const QString& path) {
    return path.startsWith(QStringLiteral("\\\\")) || path.startsWith(QStringLiteral("//"));
}

[[nodiscard]] QString new_request_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] QString new_idempotency_key() {
    return QStringLiteral("desktop-") + new_request_id();
}

[[nodiscard]] QString normalized_gpt_type_guid(const QVariantMap& partition) {
    auto gpt = partition.value(QStringLiteral("gptTypeGuid")).toString().trimmed().toLower();
    gpt.remove(QLatin1Char('{'));
    gpt.remove(QLatin1Char('}'));
    return gpt;
}

/// MSR only — Source partition bars omit these chips (EFI/Recovery stay visible).
[[nodiscard]] bool is_msr_partition(const QVariantMap& partition) {
    const auto gpt = normalized_gpt_type_guid(partition);
    if (gpt == QLatin1String("e3c9e316-0b5c-4db8-817d-f92df00215ae")) {
        return true;
    }
    const auto name = (partition.value(QStringLiteral("volumeLabel")).toString() + QLatin1Char(' ') +
                       partition.value(QStringLiteral("gptName")).toString())
                          .trimmed()
                          .toLower();
    return name == QLatin1String("msr") || name.contains(QLatin1String("microsoft reserved")) ||
           name.contains(QLatin1String("msr partition"));
}

/// EFI/MSR/Recovery and other non-data partitions (Target preview anchors; Source may hide MSR).
/// GPT partitions leave mbr_type at 0 (unset); MBR-type reserved codes must not run on them.
[[nodiscard]] bool is_reserved_partition(const QVariantMap& partition) {
    const auto gpt = normalized_gpt_type_guid(partition);
    const bool has_gpt_type = !gpt.isEmpty();
    static const char* k_reserved_gpt[] = {
        "c12a7328-f81f-11d2-ba4b-00a0c93ec93b", // EFI
        "e3c9e316-0b5c-4db8-817d-f92df00215ae", // MSR
        "de94bba4-06d1-4d40-a16a-bfd50179d6ac", // Recovery
        "5808c8aa-7e8f-42e0-85d2-e1e90434cfb3", // LDM metadata
        "af9b60a0-1431-4f62-bc68-3311714a69ad", // LDM data
        "e75caf8f-f680-4cee-afa3-b001e56efc2d", // Storage Spaces
        "00000000-0000-0000-0000-000000000000",
    };
    for (const char* guid : k_reserved_gpt) {
        if (gpt == QLatin1String(guid)) {
            return true;
        }
    }
    // MBR type 0x00 means unused; on GPT the field is never populated and stays 0.
    if (!has_gpt_type) {
        switch (partition.value(QStringLiteral("mbrType")).toInt()) {
        case 0x00:
        case 0x05:
        case 0x0F:
        case 0x12:
        case 0x27:
        case 0xEE:
        case 0xEF:
        case 0xDE:
            return true;
        default:
            break;
        }
    }
    const auto name = (partition.value(QStringLiteral("volumeLabel")).toString() +
                       QLatin1Char(' ') + partition.value(QStringLiteral("gptName")).toString())
                          .toLower();
    static const char* k_name_keys[] = {
        "microsoft reserved", "msr", "efi system", "efi ", "recovery", "winre", "oem", "diag",
        "system partition",
    };
    for (const char* key : k_name_keys) {
        if (name.contains(QLatin1String(key))) {
            return true;
        }
    }
    const auto fs = partition.value(QStringLiteral("filesystem")).toString().trimmed();
    const auto size = partition.value(QStringLiteral("sizeBytes")).toULongLong();
    return fs.isEmpty() && size > 0 && size < 256ULL * 1024 * 1024;
}

[[nodiscard]] QString style_display(const QString& style_code) {
    if (style_code == QLatin1String("mbr")) {
        return QStringLiteral("MBR");
    }
    if (style_code == QLatin1String("gpt")) {
        return QStringLiteral("GPT");
    }
    return QStringLiteral("RAW");
}

/// Volume metadata keyed by partition number (letter/label/fs from Manifest extents).
struct PartitionVolumeMeta final {
    QString letter;
    QString label;
    QString filesystem;
    qint64 size_bytes{0};
    qint64 free_bytes{-1};
    int volume_index{-1};
};

[[nodiscard]] QHash<int, PartitionVolumeMeta>
volume_meta_by_partition(const int disk_number, const QVariantList& all_volumes) {
    QHash<int, PartitionVolumeMeta> by_part;
    for (const auto& item : all_volumes) {
        const auto volume = item.toMap();
        int part_num = -1;
        for (const auto& extent_value : volume.value(QStringLiteral("extents")).toList()) {
            const auto extent = extent_value.toMap();
            if (extent.value(QStringLiteral("diskNumber")).toInt() == disk_number) {
                part_num = extent.value(QStringLiteral("partitionNumber")).toInt();
                break;
            }
        }
        if (part_num < 0) {
            continue;
        }
        auto& meta = by_part[part_num];
        const auto letter = volume.value(QStringLiteral("letter")).toString();
        if (!letter.isEmpty()) {
            meta.letter = letter;
        }
        const auto label = volume.value(QStringLiteral("label")).toString().trimmed();
        if (!label.isEmpty()) {
            meta.label = label;
        }
        const auto fs = volume.value(QStringLiteral("filesystem")).toString().trimmed();
        if (!fs.isEmpty()) {
            meta.filesystem = fs;
        }
        const auto size = volume.value(QStringLiteral("totalSizeBytes")).toLongLong();
        if (size > 0) {
            meta.size_bytes = size;
        }
        if (volume.value(QStringLiteral("freeSizeKnown")).toBool()) {
            auto free = volume.value(QStringLiteral("freeSizeBytes")).toLongLong();
            if (free < 0) {
                free = 0;
            }
            if (size > 0 && free > size) {
                free = size;
            }
            meta.free_bytes = free;
        }
        meta.volume_index = volume.value(QStringLiteral("volumeIndex")).toInt();
    }
    return by_part;
}

struct LayoutPartition final {
    qint64 offset_bytes{0};
    qint64 size_bytes{0};
    int partition_number{0};
    bool reserved{false};
    bool is_msr{false};
    /// True when a Manifest volume extent covers this partition (selected in backup).
    bool backed_up{false};
    QString letter;
    QString name;
    QString filesystem;
    qint64 free_bytes{-1};
    int volume_index{-1};
};

/// Gaps at or below this size are GPT alignment / MSR / padding (not shown as Unallocated).
/// Matches Desktop bar rules and Windows Disk Management (tiny leading free is omitted).
constexpr qint64 kUnallocatedGapThresholdBytes = 128LL * 1024LL * 1024LL;

void append_unallocated_segment(QVariantList& ui_volumes, const qint64 bytes,
                                const LocaleFormat& format) {
    if (bytes <= kUnallocatedGapThresholdBytes) {
        return;
    }
    // name is filled by QML (localized "Unallocated"); size is shown on the bar.
    ui_volumes.push_back(QVariantMap{
        {QStringLiteral("letter"), QString{}},
        {QStringLiteral("name"), QString{}},
        {QStringLiteral("size"), format.format_bytes(bytes)},
        {QStringLiteral("capacityBytes"), bytes},
        {QStringLiteral("fileSystem"), QString{}},
        {QStringLiteral("fs"), QString{}},
        {QStringLiteral("offsetBytes"), static_cast<qint64>(-1)},
        {QStringLiteral("partitionNumber"), -1},
        {QStringLiteral("volumeIndex"), -1},
        {QStringLiteral("unallocated"), true},
        {QStringLiteral("notBackedUp"), false},
    });
}

/// Source disk bar segments in physical order: data + reserved + unallocated gaps.
/// Reserved (EFI/MSR/Recovery/…) are emitted with reserved=true / isMsr so QML can
/// keep EFI/Recovery as Target anchors while omitting MSR chips on Source and Target.
[[nodiscard]] QVariantList volumes_for_source_disk(const int disk_number,
                                                   const QVariantList& partitions,
                                                   const QVariantList& all_volumes,
                                                   const qint64 disk_total,
                                                   const LocaleFormat& format) {
    const auto meta_by_part = volume_meta_by_partition(disk_number, all_volumes);

    std::vector<LayoutPartition> layout;
    layout.reserve(static_cast<std::size_t>(partitions.size()));
    for (const auto& item : partitions) {
        const auto partition = item.toMap();
        const int part_num = partition.value(QStringLiteral("partitionNumber")).toInt();
        const auto meta = meta_by_part.value(part_num);
        // Physical partition length drives layout geometry; volume size is fallback only.
        auto size = partition.value(QStringLiteral("sizeBytes")).toLongLong();
        if (size <= 0) {
            size = meta.size_bytes;
        }
        if (size <= 0) {
            continue;
        }
        LayoutPartition entry;
        entry.offset_bytes = partition.value(QStringLiteral("offsetBytes")).toLongLong();
        entry.size_bytes = size;
        entry.partition_number = part_num;
        entry.reserved = is_reserved_partition(partition);
        entry.is_msr = is_msr_partition(partition);
        // Only partitions with a Manifest volume extent were selected for backup.
        entry.backed_up = meta_by_part.contains(part_num);
        if (entry.reserved) {
            auto name = partition.value(QStringLiteral("gptName")).toString().trimmed();
            if (name.isEmpty()) {
                name = partition.value(QStringLiteral("volumeLabel")).toString().trimmed();
            }
            entry.name = localized_volume_label(name);
        } else if (entry.backed_up) {
            entry.letter = meta.letter;
            entry.name = meta.label;
            if (entry.name.isEmpty()) {
                entry.name = partition.value(QStringLiteral("volumeLabel")).toString().trimmed();
            }
            entry.name = localized_volume_label(entry.name);
            entry.filesystem = meta.filesystem;
            if (entry.filesystem.isEmpty()) {
                entry.filesystem = partition.value(QStringLiteral("filesystem")).toString();
            }
            entry.free_bytes = meta.free_bytes;
            entry.volume_index = meta.volume_index;
        }
        layout.push_back(std::move(entry));
    }
    std::sort(layout.begin(), layout.end(),
              [](const LayoutPartition& left, const LayoutPartition& right) {
                  if (left.offset_bytes != right.offset_bytes) {
                      return left.offset_bytes < right.offset_bytes;
                  }
                  return left.partition_number < right.partition_number;
              });

    QVariantList ui_volumes;
    qint64 cursor = 0;
    for (const auto& part : layout) {
        if (part.offset_bytes > cursor) {
            append_unallocated_segment(ui_volumes, part.offset_bytes - cursor, format);
        }
        const auto end = part.offset_bytes + part.size_bytes;
        if (end > cursor) {
            cursor = end;
        }
        if (part.reserved) {
            // Keep geometry for Target restore preview: fixed, non-resizable chips.
            // Source bars hide MSR via isMsr; space must not collapse into Unallocated.
            ui_volumes.push_back(QVariantMap{
                {QStringLiteral("letter"), QString{}},
                {QStringLiteral("name"), part.name},
                {QStringLiteral("title"), part.name},
                {QStringLiteral("size"), format.format_bytes(part.size_bytes)},
                {QStringLiteral("capacityBytes"), part.size_bytes},
                {QStringLiteral("fileSystem"), QString{}},
                {QStringLiteral("fs"), QString{}},
                {QStringLiteral("offsetBytes"), part.offset_bytes},
                {QStringLiteral("partitionNumber"), part.partition_number},
                {QStringLiteral("volumeIndex"), -1},
                {QStringLiteral("unallocated"), false},
                {QStringLiteral("notBackedUp"), false},
                {QStringLiteral("reserved"), true},
                {QStringLiteral("isMsr"), part.is_msr},
            });
            continue;
        }
        if (!part.backed_up) {
            // Present on disk layout but not selected into this recovery point.
            ui_volumes.push_back(QVariantMap{
                {QStringLiteral("letter"), QString{}},
                {QStringLiteral("name"), QString{}},
                {QStringLiteral("title"), QString{}},
                {QStringLiteral("size"), format.format_bytes(part.size_bytes)},
                {QStringLiteral("capacityBytes"), part.size_bytes},
                {QStringLiteral("fileSystem"), QString{}},
                {QStringLiteral("fs"), QString{}},
                {QStringLiteral("offsetBytes"), part.offset_bytes},
                {QStringLiteral("partitionNumber"), part.partition_number},
                {QStringLiteral("volumeIndex"), -1},
                {QStringLiteral("unallocated"), false},
                {QStringLiteral("notBackedUp"), true},
            });
            continue;
        }
        QVariantMap row{{QStringLiteral("letter"), part.letter},
                        {QStringLiteral("name"), part.name},
                        {QStringLiteral("title"), volume_display_title(part.name, part.letter)},
                        {QStringLiteral("size"), format.format_bytes(part.size_bytes)},
                        {QStringLiteral("capacityBytes"), part.size_bytes},
                        {QStringLiteral("fileSystem"), part.filesystem},
                        {QStringLiteral("fs"), part.filesystem},
                        {QStringLiteral("offsetBytes"), part.offset_bytes},
                        {QStringLiteral("partitionNumber"), part.partition_number},
                        {QStringLiteral("volumeIndex"), part.volume_index},
                        {QStringLiteral("unallocated"), false},
                        {QStringLiteral("notBackedUp"), false}};
        if (part.free_bytes >= 0) {
            row.insert(QStringLiteral("freeBytes"), part.free_bytes);
        }
        ui_volumes.push_back(std::move(row));
    }
    if (disk_total > cursor) {
        append_unallocated_segment(ui_volumes, disk_total - cursor, format);
    }
    return ui_volumes;
}

/// Flat Manifest volumes for volume→volume restore (source_volume_index mapping).
[[nodiscard]] QVariantList source_volumes_from_layout(const QVariantList& volumes,
                                                      const LocaleFormat& format) {
    QVariantList sorted = volumes;
    std::sort(sorted.begin(), sorted.end(), [](const QVariant& left, const QVariant& right) {
        return left.toMap().value(QStringLiteral("volumeIndex")).toInt() <
               right.toMap().value(QStringLiteral("volumeIndex")).toInt();
    });
    QVariantList out;
    out.reserve(sorted.size());
    for (const auto& item : sorted) {
        const auto volume = item.toMap();
        const auto size = volume.value(QStringLiteral("totalSizeBytes")).toLongLong();
        if (size <= 0) {
            continue;
        }
        const int volume_index = volume.value(QStringLiteral("volumeIndex")).toInt();
        if (volume_index < 0) {
            continue;
        }
        const auto letter = volume.value(QStringLiteral("letter")).toString().trimmed();
        const auto name = localized_volume_label(volume.value(QStringLiteral("label")).toString());
        const auto fs = volume.value(QStringLiteral("filesystem")).toString().trimmed();
        QVariantMap row{{QStringLiteral("volumeIndex"), volume_index},
                        {QStringLiteral("letter"), letter},
                        {QStringLiteral("name"), name},
                        {QStringLiteral("title"), volume_display_title(name, letter)},
                        {QStringLiteral("size"), format.format_bytes(size)},
                        {QStringLiteral("capacityBytes"), size},
                        {QStringLiteral("fileSystem"), fs},
                        {QStringLiteral("fs"), fs}};
        if (volume.value(QStringLiteral("freeSizeKnown")).toBool()) {
            auto free = volume.value(QStringLiteral("freeSizeBytes")).toLongLong();
            if (free < 0) {
                free = 0;
            }
            if (free > size) {
                free = size;
            }
            row.insert(QStringLiteral("freeBytes"), free);
        }
        out.push_back(std::move(row));
    }
    return out;
}

/// Hierarchical layout (disks + volumes) → Restore Source DiskRows (old project shape).
[[nodiscard]] QVariantList source_disks_from_layout(const QVariantList& disks,
                                                    const QVariantList& volumes,
                                                    const LocaleFormat& format) {
    QVariantList sorted = disks;
    std::sort(sorted.begin(), sorted.end(), [](const QVariant& left, const QVariant& right) {
        return left.toMap().value(QStringLiteral("diskNumber")).toInt() <
               right.toMap().value(QStringLiteral("diskNumber")).toInt();
    });
    QVariantList out;
    out.reserve(sorted.size());
    for (const auto& item : sorted) {
        const auto disk = item.toMap();
        const int disk_number = disk.value(QStringLiteral("diskNumber")).toInt();
        const auto disk_total = disk.value(QStringLiteral("diskSizeBytes")).toLongLong();
        const auto style = style_display(disk.value(QStringLiteral("partitionStyle")).toString());
        const auto ui_volumes =
            volumes_for_source_disk(disk_number, disk.value(QStringLiteral("partitions")).toList(),
                                    volumes, disk_total, format);
        out.push_back(QVariantMap{
            {QStringLiteral("diskNumber"), disk_number},
            {QStringLiteral("name"), QStringLiteral("Disk %1").arg(disk_number)},
            {QStringLiteral("partitionStyle"), style},
            {QStringLiteral("type"), QStringLiteral("Basic (%1)").arg(style)},
            {QStringLiteral("size"), format.format_bytes(disk_total)},
            {QStringLiteral("capacityBytes"), disk_total},
            {QStringLiteral("isSystemDisk"), false},
            {QStringLiteral("volumes"), ui_volumes},
        });
    }
    return out;
}

} // namespace

void ServiceClient::start_repository_query() {
    reset_repository();
    repository_loading_ = true;
    emit repositoryChanged();
    emit loadingChanged();

    const auto request_id = new_request_id();
    repository_request_id_ = request_id;
    requested_token_.reset();
    const std::optional<QString> connection_id =
        selected_repository_connection_id_.isEmpty()
            ? std::nullopt
            : std::optional{selected_repository_connection_id_};
    const auto body = encode_recovery_point_request(request_id, std::nullopt, connection_id);
    const auto started = coordinator_->begin_request(
        request_id, body,
        [this](const QByteArray& frame_body) { return handle_recovery_point_frame(frame_body); },
        kRepositoryQueryDeadlineMilliseconds);
    if (!started) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
    }
}

RequestDisposition ServiceClient::handle_recovery_point_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_repository_failure_response(root)) {
        finish_repository_failure(QStringLiteral("repository.query_failed"));
        return RequestDisposition::kFinished;
    }

    RecoveryPointPage page;
    if (!parse_recovery_point_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    const std::optional<QString> expected_connection =
        selected_repository_connection_id_.isEmpty()
            ? std::nullopt
            : std::optional{selected_repository_connection_id_};
    if (page.repository_connection_id != expected_connection) {
        return RequestDisposition::kProtocolError;
    }
    if (!page.configured) {
        if (requested_token_ || !pending_recovery_points_.isEmpty()) {
            return RequestDisposition::kProtocolError;
        }
        repository_loading_ = false;
        repository_request_id_.clear();
        emit repositoryChanged();
        emit loadingChanged();
        return RequestDisposition::kFinished;
    }
    if ((!repository_uuid_.isEmpty() && page.repository_uuid != repository_uuid_) ||
        (page.continuation_token && page.continuation_token == requested_token_) ||
        pending_recovery_points_.size() + page.items.size() > kMaximumRecoveryPoints) {
        return RequestDisposition::kProtocolError;
    }
    repository_uuid_ = page.repository_uuid;
    for (auto& item : page.items) {
        const auto file_uuid = item.toMap().value(QStringLiteral("fileUuid")).toString();
        if (!last_file_uuid_.isEmpty() && file_uuid <= last_file_uuid_) {
            return RequestDisposition::kProtocolError;
        }
        last_file_uuid_ = file_uuid;
        pending_recovery_points_.push_back(std::move(item));
    }
    if (page.continuation_token) {
        requested_token_ = page.continuation_token;
        const auto next_id = new_request_id();
        const auto next_body =
            encode_recovery_point_request(next_id, requested_token_, expected_connection);
        if (!coordinator_->continue_request(request_id, next_id, next_body,
                                            kRepositoryQueryDeadlineMilliseconds)) {
            return RequestDisposition::kProtocolError;
        }
        repository_request_id_ = next_id;
        return RequestDisposition::kContinue;
    }
    recovery_points_.set_rows(recovery_points_from_variant_list(pending_recovery_points_));
    pending_recovery_points_.clear();
    repository_configured_ = true;
    repository_loading_ = false;
    repository_request_id_.clear();
    requested_token_.reset();
    emit repositoryChanged();
    emit loadingChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_failure(const QString& message_code) {
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_loading_ = false;
    repository_request_id_.clear();
    repository_error_code_ = message_code;
    // Keep the last complete catalog snapshot. A transient Service or network failure must not
    // replace valid UI data with an empty repository.
    emit repositoryChanged();
    emit loadingChanged();
}

void ServiceClient::reset_repository() {
    repository_uuid_.clear();
    repository_error_code_.clear();
    recovery_points_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_configured_ = false;
    repository_loading_ = false;
    repository_request_id_.clear();
    reset_recovery_point_layout();
    emit repositoryChanged();
    emit loadingChanged();
}

bool ServiceClient::recoveryPointLayoutLoading() const noexcept {
    return recovery_point_layout_loading_;
}

QVariantList ServiceClient::recoveryPointSourceDisks() const {
    return recovery_point_source_disks_;
}

QVariantList ServiceClient::recoveryPointSourceVolumes() const {
    return recovery_point_source_volumes_;
}

QString ServiceClient::recoveryPointLayoutErrorText() const {
    return recovery_point_layout_error_code_.isEmpty()
               ? QString{}
               : localize_message_code(recovery_point_layout_error_code_);
}

void ServiceClient::loadRecoveryPointLayout(const QString& recovery_point_id,
                                            const QString& archive_password) {
    if (recovery_point_id.isEmpty()) {
        reset_recovery_point_layout();
        return;
    }
    if (state_ != State::kReady || selected_repository_connection_id_.isEmpty()) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
        return;
    }
    // Replace any in-flight layout query for a different checkpoint.
    recovery_point_layout_error_code_.clear();
    recovery_point_source_disks_.clear();
    recovery_point_source_volumes_.clear();
    recovery_point_layout_loading_ = true;
    recovery_point_layout_recovery_point_id_ = recovery_point_id;
    emit recoveryPointLayoutChanged();
    emit loadingChanged();

    const auto request_id = new_request_id();
    recovery_point_layout_request_id_ = request_id;
    const auto body = encode_recovery_point_layout_request(
        request_id, selected_repository_connection_id_, recovery_point_id, archive_password);
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_recovery_point_layout_frame(frame_body);
        });
    if (!started) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
    }
}

RequestDisposition ServiceClient::handle_recovery_point_layout_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    // Superseded layout requests must not mutate the active selection state.
    if (request_id.isEmpty() || request_id != recovery_point_layout_request_id_) {
        return RequestDisposition::kFinished;
    }
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_recovery_point_layout_failure_response(root)) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
        return RequestDisposition::kFinished;
    }
    QVariantMap layout;
    if (!parse_recovery_point_layout_response(root, layout)) {
        return RequestDisposition::kProtocolError;
    }
    const auto connection_id = layout.value(QStringLiteral("repositoryConnectionId")).toString();
    const auto recovery_point_id = layout.value(QStringLiteral("recoveryPointId")).toString();
    if (connection_id != selected_repository_connection_id_ ||
        recovery_point_id != recovery_point_layout_recovery_point_id_) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
        return RequestDisposition::kFinished;
    }
    const auto layout_volumes = layout.value(QStringLiteral("volumes")).toList();
    recovery_point_source_disks_ = source_disks_from_layout(
        layout.value(QStringLiteral("disks")).toList(), layout_volumes, format_);
    recovery_point_source_volumes_ = source_volumes_from_layout(layout_volumes, format_);
    if (recovery_point_source_disks_.isEmpty() || recovery_point_source_volumes_.isEmpty()) {
        finish_recovery_point_layout_failure(QStringLiteral("recovery_point.layout_failed"));
        return RequestDisposition::kFinished;
    }
    recovery_point_layout_loading_ = false;
    recovery_point_layout_request_id_.clear();
    recovery_point_layout_error_code_.clear();
    emit recoveryPointLayoutChanged();
    emit loadingChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_recovery_point_layout_failure(const QString& message_code) {
    recovery_point_source_disks_.clear();
    recovery_point_source_volumes_.clear();
    recovery_point_layout_loading_ = false;
    recovery_point_layout_request_id_.clear();
    recovery_point_layout_error_code_ = message_code;
    emit recoveryPointLayoutChanged();
    emit loadingChanged();
}

void ServiceClient::reset_recovery_point_layout() {
    const bool had_state =
        recovery_point_layout_loading_ || !recovery_point_source_disks_.isEmpty() ||
        !recovery_point_source_volumes_.isEmpty() || !recovery_point_layout_error_code_.isEmpty() ||
        !recovery_point_layout_recovery_point_id_.isEmpty();
    recovery_point_source_disks_.clear();
    recovery_point_source_volumes_.clear();
    recovery_point_layout_loading_ = false;
    recovery_point_layout_request_id_.clear();
    recovery_point_layout_recovery_point_id_.clear();
    recovery_point_layout_error_code_.clear();
    if (had_state) {
        emit recoveryPointLayoutChanged();
        emit loadingChanged();
    }
}

QString ServiceClient::selectedRepositoryConnectionId() const {
    return selected_repository_connection_id_;
}

bool ServiceClient::repositoryCommandBusy() const noexcept { return repository_command_busy_; }

QString ServiceClient::repositoryCommandErrorText() const {
    return repository_command_error_code_.isEmpty()
               ? QString{}
               : localize_message_code(repository_command_error_code_);
}

QString ServiceClient::repositoryCommandErrorCode() const { return repository_command_error_code_; }

bool ServiceClient::repositoryDirectoriesLoading() const noexcept {
    return repository_directories_loading_;
}

QVariantList ServiceClient::repositoryDirectories() const { return repository_directories_; }

QString ServiceClient::repositoryDirectoriesErrorText() const {
    return repository_directories_error_code_.isEmpty()
               ? QString{}
               : localize_message_code(repository_directories_error_code_);
}

void ServiceClient::selectRepositoryConnection(const QString& connection_id) {
    if (connection_id.isEmpty() || !connections_.find(connection_id)) {
        return;
    }
    const bool changed = selected_repository_connection_id_ != connection_id;
    selected_repository_connection_id_ = connection_id;
    if (changed) {
        emit repositoryChanged();
    }
    refreshRepository();
}

void ServiceClient::addRepositoryConnection(const QString& display_name, const QString& locator,
                                            const QString& network_username,
                                            const QString& network_password,
                                            const QString& network_domain) {
    start_repository_input_command(kAddRepositoryConnectionRequestKind, display_name, locator,
                                   network_username, network_password, network_domain);
}

void ServiceClient::importRepositoryConnection(const QString& display_name, const QString& locator,
                                               const QString& network_username,
                                               const QString& network_password,
                                               const QString& network_domain) {
    start_repository_input_command(kImportRepositoryConnectionRequestKind, display_name, locator,
                                   network_username, network_password, network_domain);
}

void ServiceClient::connectRepositoryLocation(const QString& locator,
                                              const QString& network_username,
                                              const QString& network_password,
                                              const QString& network_domain) {
    start_repository_input_command(kConnectRepositoryLocationRequestKind,
                                   QStringLiteral("network-probe"), locator, network_username,
                                   network_password, network_domain);
}

void ServiceClient::testRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kTestRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::begin_next_repository_refresh() {
    if (!repository_refresh_running_) {
        return;
    }
    if (!connected() || repository_refresh_queue_.isEmpty()) {
        stop_repository_refresh();
        return;
    }
    repository_refresh_current_id_ = repository_refresh_queue_.takeFirst();
    if (!connections_.find(repository_refresh_current_id_)) {
        begin_next_repository_refresh();
        return;
    }
    // A prior command must already be finished before the next probe starts.
    // If not, put the id back and stop rather than dropping the connection.
    if (repository_command_busy_) {
        repository_refresh_queue_.prepend(repository_refresh_current_id_);
        repository_refresh_current_id_.clear();
        connections_.clear_refreshing();
        stop_repository_refresh();
        return;
    }
    connections_.set_refreshing(repository_refresh_current_id_, true);
    connections_.clear_probe_error(repository_refresh_current_id_);
    emit connectionsChanged();
    start_repository_resource_command(kTestRepositoryConnectionRequestKind,
                                      repository_refresh_current_id_);
    // start_repository_resource_command returns without setting busy when the
    // connection disappeared between find() and send; advance to the next row.
    if (!repository_command_busy_) {
        begin_next_repository_refresh();
    }
}

void ServiceClient::request_connection_snapshot_after_probe() {
    if (!repository_refresh_running_) {
        return;
    }
    repository_refresh_waiting_for_snapshot_ = true;
    if (!connections_loading_) {
        start_connection_query();
    }
}

void ServiceClient::stop_repository_refresh() {
    const bool changed = repository_refresh_running_ || repository_refresh_waiting_for_snapshot_ ||
                         !repository_refresh_current_id_.isEmpty() ||
                         !repository_refresh_queue_.isEmpty();
    repository_refresh_running_ = false;
    repository_refresh_waiting_for_snapshot_ = false;
    repository_refresh_current_id_.clear();
    repository_refresh_queue_.clear();
    connections_.clear_refreshing();
    if (changed) {
        emit connectionsChanged();
    }
}

void ServiceClient::setDefaultRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kSetDefaultRepositoryRequestKind, connection_id);
}

void ServiceClient::removeRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kRemoveRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::start_repository_input_command(const int request_kind,
                                                   const QString& display_name,
                                                   const QString& locator,
                                                   const QString& network_username,
                                                   const QString& network_password,
                                                   const QString& network_domain) {
    if (!connected() || repository_command_busy_ || display_name.trimmed().isEmpty() ||
        locator.trimmed().isEmpty()) {
        return;
    }
    repository_command_busy_ = true;
    repository_command_error_code_.clear();
    repository_command_kind_ = request_kind;
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    emit repositoryCommandChanged();
    const auto body = encode_repository_connection_input_request(
        repository_command_request_id_, repository_command_idempotency_key_, request_kind,
        display_name.trimmed(), locator.trimmed(), network_username, network_password,
        network_domain);
    const auto deadline = request_kind == kConnectRepositoryLocationRequestKind
                              ? kRepositoryProbeDeadlineMilliseconds
                              : ServiceRequestCoordinator::kDefaultDeadlineMilliseconds;
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body,
        [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        },
        deadline);
    if (!started) {
        finish_repository_command_failure(QStringLiteral("service.send_failed"));
    }
}

void ServiceClient::start_repository_resource_command(const int request_kind,
                                                      const QString& connection_id) {
    if (!connected() || repository_command_busy_ || !connections_.find(connection_id)) {
        return;
    }
    repository_command_busy_ = true;
    repository_command_error_code_.clear();
    repository_command_kind_ = request_kind;
    repository_command_connection_id_ = connection_id;
    if (request_kind == kTestRepositoryConnectionRequestKind) {
        connections_.clear_probe_error(connection_id);
    }
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    emit repositoryCommandChanged();
    const auto body = encode_repository_connection_resource_request(
        repository_command_request_id_, repository_command_idempotency_key_, request_kind,
        connection_id);
    const auto deadline = request_kind == kTestRepositoryConnectionRequestKind
                              ? kRepositoryProbeDeadlineMilliseconds
                              : ServiceRequestCoordinator::kDefaultDeadlineMilliseconds;
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body,
        [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        },
        deadline);
    if (!started) {
        finish_repository_command_failure(QStringLiteral("service.send_failed"));
    }
}

RequestDisposition ServiceClient::handle_repository_command_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, repository_command_request_id_, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, repository_command_kind_)) {
        const bool location_probe =
            repository_command_kind_ == kConnectRepositoryLocationRequestKind;
        const bool refresh_probe = repository_refresh_running_ &&
                                   repository_command_kind_ == kTestRepositoryConnectionRequestKind;
        const auto message_code = root.value(QStringLiteral("message_code")).toString();
        finish_repository_command_failure(
            message_code.isEmpty() ? QStringLiteral("service.request_failed") : message_code);
        if (!refresh_probe && !location_probe) {
            refreshConnections();
        }
        return RequestDisposition::kFinished;
    }
    CommandAck acknowledgement;
    if (!parse_command_ack_response(root, repository_command_kind_, acknowledgement)) {
        return RequestDisposition::kProtocolError;
    }
    const auto completed_kind = repository_command_kind_;
    const bool location_probe = completed_kind == kConnectRepositoryLocationRequestKind;
    const bool refresh_probe =
        repository_refresh_running_ && completed_kind == kTestRepositoryConnectionRequestKind;
    if (completed_kind == kRemoveRepositoryConnectionRequestKind &&
        acknowledgement.resource_id == selected_repository_connection_id_) {
        selected_repository_connection_id_.clear();
        reset_repository();
    } else if (acknowledgement.has_resource_id && !refresh_probe && !location_probe) {
        selected_repository_connection_id_ = acknowledgement.resource_id;
    }
    if (location_probe && acknowledgement.has_resource_id) {
        repository_location_token_ = acknowledgement.resource_id;
    }
    if (completed_kind == kTestRepositoryConnectionRequestKind &&
        !repository_command_connection_id_.isEmpty()) {
        connections_.clear_probe_error(repository_command_connection_id_);
        if (acknowledgement.free_bytes) {
            connections_.set_free_bytes(repository_command_connection_id_,
                                        *acknowledgement.free_bytes);
        }
        emit connectionsChanged();
    }
    reset_repository_command();
    if (refresh_probe) {
        request_connection_snapshot_after_probe();
    } else if (!location_probe) {
        refreshConnections();
    }
    if (!location_probe) {
        emit repositoryChanged();
    } else if (!repository_location_token_.isEmpty()) {
        refresh_repository_directories(repository_location_token_);
    }
    return RequestDisposition::kFinished;
}

void ServiceClient::refresh_repository_directories(const QString& location_token) {
    if (!connected() || location_token.isEmpty() || repository_directories_loading_) {
        return;
    }
    repository_directories_loading_ = true;
    repository_directories_.clear();
    repository_directories_error_code_.clear();
    repository_directories_request_id_ = new_request_id();
    emit repositoryDirectoriesChanged();
    const auto body = encode_repository_directory_list_request(repository_directories_request_id_,
                                                               location_token);
    const auto started = coordinator_->begin_request(
        repository_directories_request_id_, body,
        [this](const QByteArray& frame_body) {
            return handle_repository_directories_frame(frame_body);
        },
        kRepositoryProbeDeadlineMilliseconds);
    if (!started) {
        finish_repository_directories_failure(QStringLiteral("service.send_failed"));
    }
}

RequestDisposition ServiceClient::handle_repository_directories_frame(const QByteArray& body) {
    QJsonObject root;
    if (!parse_response_root(body, repository_directories_request_id_, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kListRepositoryDirectoriesRequestKind)) {
        const auto message_code = root.value(QStringLiteral("message_code")).toString();
        finish_repository_directories_failure(
            message_code.isEmpty() ? QStringLiteral("service.request_failed") : message_code);
        return RequestDisposition::kFinished;
    }
    FileBrowsePage page;
    if (!parse_repository_directory_list_response(root, page)) {
        return RequestDisposition::kProtocolError;
    }
    QVariantList directories;
    directories.reserve(page.items.size());
    for (const auto& value : page.items) {
        const auto item = value.toMap();
        if (item.value(QStringLiteral("isDirectory")).toBool()) {
            directories.push_back(item.value(QStringLiteral("displayName")).toString());
        }
    }
    repository_directories_ = std::move(directories);
    repository_directories_loading_ = false;
    repository_directories_request_id_.clear();
    repository_directories_error_code_.clear();
    emit repositoryDirectoriesChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_directories_failure(const QString& message_code) {
    repository_directories_loading_ = false;
    repository_directories_request_id_.clear();
    repository_directories_error_code_ = message_code;
    emit repositoryDirectoriesChanged();
}

void ServiceClient::finish_repository_command_failure(const QString& message_code) {
    const bool test_probe = repository_command_kind_ == kTestRepositoryConnectionRequestKind;
    const bool refresh_probe = repository_refresh_running_ && test_probe;
    // Row-level error for Test/Refresh: store message_code; model localizes for display.
    if (test_probe && !repository_command_connection_id_.isEmpty()) {
        const auto code = message_code.isEmpty() ? QStringLiteral("service.request_failed")
                                                 : message_code;
        connections_.set_probe_error(repository_command_connection_id_, code);
        connections_.set_refreshing(repository_command_connection_id_, false);
        connections_.set_free_bytes(repository_command_connection_id_, std::nullopt);
        emit connectionsChanged();
    }
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_connection_id_.clear();
    repository_command_error_code_ = message_code;
    emit repositoryCommandChanged();
    if (refresh_probe) {
        request_connection_snapshot_after_probe();
    }
}

void ServiceClient::reset_repository_command() {
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_connection_id_.clear();
    repository_command_error_code_.clear();
    emit repositoryCommandChanged();
}

bool ServiceClient::deletePlanBusy() const noexcept { return delete_plan_busy_; }

QVariantMap ServiceClient::deletePlan() const { return delete_plan_; }

QString ServiceClient::deletePlanErrorText() const {
    return delete_plan_error_code_.isEmpty() ? QString{}
                                             : localize_message_code(delete_plan_error_code_);
}

void ServiceClient::clearDeletePlan() {
    if (!delete_plan_busy_ && delete_plan_.isEmpty() && delete_plan_error_code_.isEmpty()) {
        return;
    }
    delete_plan_busy_ = false;
    delete_plan_request_id_.clear();
    execute_delete_request_id_.clear();
    execute_delete_idempotency_key_.clear();
    delete_plan_error_code_.clear();
    delete_plan_.clear();
    emit deletePlanChanged();
}

bool ServiceClient::planDeleteRecoveryPoint(const QString& recovery_point_id,
                                            const QString& archive_password) {
    if (state_ != State::kReady || delete_plan_busy_ || recovery_point_id.isEmpty() ||
        selected_repository_connection_id_.isEmpty()) {
        return false;
    }
    delete_plan_.clear();
    delete_plan_error_code_.clear();
    delete_plan_busy_ = true;
    delete_plan_request_id_ = new_request_id();
    emit deletePlanChanged();
    const auto body = encode_plan_delete_recovery_points_request(
        delete_plan_request_id_, selected_repository_connection_id_, recovery_point_id,
        archive_password);
    const auto started = coordinator_->begin_request(
        delete_plan_request_id_, body,
        [this](const QByteArray& frame_body) { return handle_plan_delete_frame(frame_body); });
    if (!started) {
        finish_plan_delete_failure(QStringLiteral("repository.query_failed"));
        return false;
    }
    return true;
}

bool ServiceClient::executeDeletePlan() {
    if (state_ != State::kReady || delete_plan_busy_ ||
        delete_plan_.value(QStringLiteral("planToken")).toString().isEmpty()) {
        return false;
    }
    const auto plan_token = delete_plan_.value(QStringLiteral("planToken")).toString();
    delete_plan_busy_ = true;
    delete_plan_error_code_.clear();
    execute_delete_request_id_ = new_request_id();
    execute_delete_idempotency_key_ = new_idempotency_key();
    emit deletePlanChanged();
    const auto body = encode_execute_delete_plan_request(
        execute_delete_request_id_, execute_delete_idempotency_key_, plan_token, true);
    const auto started = coordinator_->begin_request(
        execute_delete_request_id_, body, [this](const QByteArray& frame_body) {
            return handle_execute_delete_plan_frame(frame_body);
        });
    if (!started) {
        finish_execute_delete_failure(QStringLiteral("service.request_failed"));
        return false;
    }
    return true;
}

RequestDisposition ServiceClient::handle_plan_delete_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    if (request_id.isEmpty() || request_id != delete_plan_request_id_) {
        return RequestDisposition::kFinished;
    }
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kPlanDeleteRecoveryPointsRequestKind)) {
        const auto code = root.value(QStringLiteral("message_code")).toString();
        finish_plan_delete_failure(code.isEmpty() ? QStringLiteral("repository.query_failed")
                                                  : code);
        return RequestDisposition::kFinished;
    }
    QVariantMap plan;
    if (!parse_delete_plan_response(root, plan)) {
        return RequestDisposition::kProtocolError;
    }
    if (plan.value(QStringLiteral("repositoryConnectionId")).toString() !=
        selected_repository_connection_id_) {
        finish_plan_delete_failure(QStringLiteral("repository.query_failed"));
        return RequestDisposition::kFinished;
    }
    // Display-only retained estimate from currently loaded RP list (not authority).
    const auto target_count = plan.value(QStringLiteral("targetCount")).toLongLong();
    const auto loaded = recovery_points_.rowCount();
    if (loaded >= target_count) {
        plan.insert(QStringLiteral("retainedCount"), static_cast<qint64>(loaded - target_count));
    }
    delete_plan_ = std::move(plan);
    delete_plan_busy_ = false;
    delete_plan_request_id_.clear();
    delete_plan_error_code_.clear();
    emit deletePlanChanged();
    emit deletePlanReady();
    return RequestDisposition::kFinished;
}

RequestDisposition ServiceClient::handle_execute_delete_plan_frame(const QByteArray& body) {
    const auto request_id = extract_response_request_id(body);
    if (request_id.isEmpty() || request_id != execute_delete_request_id_) {
        return RequestDisposition::kFinished;
    }
    QJsonObject root;
    if (!parse_response_root(body, request_id, root)) {
        return RequestDisposition::kProtocolError;
    }
    if (is_command_failure_response(root, kExecuteDeletePlanRequestKind)) {
        const auto code = root.value(QStringLiteral("message_code")).toString();
        finish_execute_delete_failure(code.isEmpty() ? QStringLiteral("service.request_failed")
                                                     : code);
        return RequestDisposition::kFinished;
    }
    CommandAck acknowledgement;
    if (!parse_command_ack_response(root, kExecuteDeletePlanRequestKind, acknowledgement)) {
        return RequestDisposition::kProtocolError;
    }
    delete_plan_busy_ = false;
    execute_delete_request_id_.clear();
    execute_delete_idempotency_key_.clear();
    delete_plan_.clear();
    delete_plan_error_code_.clear();
    emit deletePlanChanged();
    emit deleteExecuted();
    refreshRepository();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_plan_delete_failure(const QString& message_code) {
    delete_plan_busy_ = false;
    delete_plan_request_id_.clear();
    delete_plan_.clear();
    delete_plan_error_code_ = message_code;
    emit deletePlanChanged();
    emit deletePlanFailed(localize_message_code(message_code));
}

void ServiceClient::finish_execute_delete_failure(const QString& message_code) {
    delete_plan_busy_ = false;
    execute_delete_request_id_.clear();
    execute_delete_idempotency_key_.clear();
    delete_plan_error_code_ = message_code;
    emit deletePlanChanged();
    emit deletePlanFailed(localize_message_code(message_code));
}

QVariantList ServiceClient::listLocalRepositoryDrives() const {
    QVariantList drives;
    QSet<QString> seen;
    for (const auto& disk_value : sources_.disksTree()) {
        const auto volumes = disk_value.toMap().value(QStringLiteral("volumes")).toList();
        for (const auto& volume_value : volumes) {
            auto letter = volume_value.toMap().value(QStringLiteral("letter")).toString().trimmed();
            if (letter.isEmpty()) {
                letter =
                    volume_value.toMap().value(QStringLiteral("mountLetter")).toString().trimmed();
            }
            if (letter.isEmpty()) {
                continue;
            }
            const auto root = letter.left(1).toUpper() + QStringLiteral(":\\");
            if (seen.contains(root)) {
                continue;
            }
            seen.insert(root);
            drives.push_back(root);
        }
    }
    std::sort(drives.begin(), drives.end(), [](const QVariant& left, const QVariant& right) {
        return left.toString().compare(right.toString(), Qt::CaseInsensitive) < 0;
    });
    return drives;
}

QString ServiceClient::formatBytes(const qint64 bytes) const { return format_.format_bytes(bytes); }

namespace {

[[nodiscard]] QVariantMap inventory_volume(const SourceInventoryModel& sources,
                                           const QString& locator) {
    const auto trimmed = locator.trimmed();
    if (trimmed.size() < 2 || trimmed.at(1) != QLatin1Char(':')) {
        return {};
    }
    const auto wanted = trimmed.left(1).toUpper();
    for (const auto& disk_value : sources.disksTree()) {
        const auto disk = disk_value.toMap();
        const auto volumes = disk.value(QStringLiteral("volumes")).toList();
        for (const auto& volume_value : volumes) {
            auto volume = volume_value.toMap();
            auto letter = volume.value(QStringLiteral("letter")).toString();
            if (letter.isEmpty()) {
                letter = volume.value(QStringLiteral("mountLetter")).toString();
            }
            if (letter.left(1).compare(wanted, Qt::CaseInsensitive) == 0) {
                // Parent disk number for restore drag-drop (archive-on-target) checks.
                volume.insert(QStringLiteral("diskNumber"),
                              disk.value(QStringLiteral("diskNumber")));
                return volume;
            }
        }
    }
    return {};
}

[[nodiscard]] QString default_repository_locator(const RepositoryConnectionModel& connections) {
    const auto connection_id = connections.default_connection_id();
    if (connection_id.isEmpty()) {
        return {};
    }
    const auto row = connections.find(connection_id);
    if (!row) {
        return {};
    }
    return row->locator.trimmed();
}

} // namespace

qint64 ServiceClient::repositoryHostFreeBytes() const {
    // Free space on unique volumes that host registered repository locators.
    qint64 free_bytes = 0;
    QSet<QString> seen_roots;
    const int rows = connections_.rowCount();
    for (int row = 0; row < rows; ++row) {
        const auto locator =
            connections_.data(connections_.index(row, 0), RepositoryConnectionModel::LocatorRole)
                .toString();
        const auto key = locator.left(1).toUpper();
        const auto volume = inventory_volume(sources_, locator);
        if (volume.isEmpty() || seen_roots.contains(key)) {
            continue;
        }
        seen_roots.insert(key);
        free_bytes += volume.value(QStringLiteral("freeBytes")).toLongLong();
    }
    return free_bytes;
}

qint64 ServiceClient::repositoryHostUsedBytes() const {
    // Used space = used size of each unique host volume (not AegraRepo folder size).
    qint64 used_bytes = 0;
    QSet<QString> seen_roots;
    const int rows = connections_.rowCount();
    for (int row = 0; row < rows; ++row) {
        const auto locator =
            connections_.data(connections_.index(row, 0), RepositoryConnectionModel::LocatorRole)
                .toString();
        const auto key = locator.left(1).toUpper();
        const auto volume = inventory_volume(sources_, locator);
        if (volume.isEmpty() || seen_roots.contains(key)) {
            continue;
        }
        seen_roots.insert(key);
        const auto capacity = volume.value(QStringLiteral("capacityBytes")).toLongLong();
        const auto free = volume.value(QStringLiteral("freeBytes")).toLongLong();
        used_bytes += (std::max)(qint64{0}, capacity - free);
    }
    return used_bytes;
}

qint64 ServiceClient::freeBytesForLocator(const QString& locator) const {
    return inventory_volume(sources_, locator).value(QStringLiteral("freeBytes")).toLongLong();
}

QString ServiceClient::freeSpaceTextForLocator(const QString& locator) const {
    // Prefer free space from last Online Test (GetDiskFreeSpaceExW via Service),
    // including UNC after a successful network probe.
    if (const auto probed = connections_.free_bytes_for_locator(locator.trimmed())) {
        return format_.format_bytes(*probed);
    }
    if (is_unc_path(locator.trimmed())) {
        return QStringLiteral("—");
    }
    return format_.format_bytes(freeBytesForLocator(locator));
}

int ServiceClient::defaultRepositoryHostDiskNumber() const {
    const auto locator = default_repository_locator(connections_);
    if (locator.isEmpty() || is_unc_path(locator)) {
        return -1;
    }
    const auto volume = inventory_volume(sources_, locator);
    if (volume.isEmpty() || !volume.contains(QStringLiteral("diskNumber"))) {
        return -1;
    }
    bool ok = false;
    const auto disk_number = volume.value(QStringLiteral("diskNumber")).toInt(&ok);
    return ok && disk_number >= 0 ? disk_number : -1;
}

QString ServiceClient::defaultRepositoryHostVolumeSourceId() const {
    const auto locator = default_repository_locator(connections_);
    if (locator.isEmpty() || is_unc_path(locator)) {
        return {};
    }
    return inventory_volume(sources_, locator).value(QStringLiteral("sourceId")).toString();
}

} // namespace aegra::desktop
