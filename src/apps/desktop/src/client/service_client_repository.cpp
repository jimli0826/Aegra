#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/locale_format.h"
#include "locale/message_code_map.h"

#include <QHash>
#include <QJsonObject>
#include <QUuid>

#include <algorithm>

namespace aegra::desktop {
namespace {

constexpr qsizetype kMaximumRecoveryPoints = 10'000;

[[nodiscard]] QString new_request_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] QString new_idempotency_key() {
    return QStringLiteral("desktop-") + new_request_id();
}

/// Hide MSR/EFI/Recovery and other non-data partitions from Restore Source bars.
/// GPT partitions leave mbr_type at 0 (unset); MBR-type reserved codes must not run on them.
[[nodiscard]] bool is_reserved_partition(const QVariantMap& partition) {
    auto gpt = partition.value(QStringLiteral("gptTypeGuid")).toString().trimmed().toLower();
    gpt.remove(QLatin1Char('{'));
    gpt.remove(QLatin1Char('}'));
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
    const auto name = (partition.value(QStringLiteral("volumeLabel")).toString() + QLatin1Char(' ') +
                       partition.value(QStringLiteral("gptName")).toString())
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

/// Old volumesForSourceDisk: partitions (order) + volumes via extents for letter/label/fs/size.
[[nodiscard]] QVariantList volumes_for_source_disk(const int disk_number,
                                                  const QVariantList& partitions,
                                                  const QVariantList& all_volumes,
                                                  const qint64 disk_total,
                                                  const LocaleFormat& format) {
    QHash<int, QString> letter_by_part;
    QHash<int, QString> label_by_part;
    QHash<int, QString> fs_by_part;
    QHash<int, qint64> size_by_part;
    QHash<int, int> volume_index_by_part;
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
        const auto letter = volume.value(QStringLiteral("letter")).toString();
        if (!letter.isEmpty()) {
            letter_by_part.insert(part_num, letter);
        }
        const auto label = volume.value(QStringLiteral("label")).toString().trimmed();
        if (!label.isEmpty()) {
            label_by_part.insert(part_num, label);
        }
        const auto fs = volume.value(QStringLiteral("filesystem")).toString().trimmed();
        if (!fs.isEmpty()) {
            fs_by_part.insert(part_num, fs);
        }
        const auto size = volume.value(QStringLiteral("totalSizeBytes")).toLongLong();
        if (size > 0) {
            size_by_part.insert(part_num, size);
        }
        volume_index_by_part.insert(part_num, volume.value(QStringLiteral("volumeIndex")).toInt());
    }

    QVariantList ui_volumes;
    for (const auto& item : partitions) {
        const auto partition = item.toMap();
        if (is_reserved_partition(partition)) {
            continue;
        }
        const int part_num = partition.value(QStringLiteral("partitionNumber")).toInt();
        auto size = size_by_part.value(part_num, 0);
        if (size <= 0) {
            size = partition.value(QStringLiteral("sizeBytes")).toLongLong();
        }
        if (size <= 0) {
            continue;
        }
        const auto letter = letter_by_part.value(part_num);
        auto name = label_by_part.value(part_num);
        if (name.isEmpty()) {
            name = partition.value(QStringLiteral("volumeLabel")).toString().trimmed();
        }
        if (name.isEmpty()) {
            name = QStringLiteral("New Volume");
        }
        auto fs = fs_by_part.value(part_num);
        if (fs.isEmpty()) {
            fs = partition.value(QStringLiteral("filesystem")).toString();
        }
        ui_volumes.push_back(QVariantMap{
            {QStringLiteral("letter"), letter},
            {QStringLiteral("name"), name},
            {QStringLiteral("size"), format.format_bytes(size)},
            {QStringLiteral("capacityBytes"), size},
            {QStringLiteral("fileSystem"), fs},
            {QStringLiteral("fs"), fs},
            {QStringLiteral("partitionNumber"), part_num},
            {QStringLiteral("volumeIndex"), volume_index_by_part.value(part_num, -1)},
        });
    }
    Q_UNUSED(disk_total);
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
        auto name = volume.value(QStringLiteral("label")).toString().trimmed();
        if (name.isEmpty()) {
            name = QStringLiteral("New Volume");
        }
        const auto fs = volume.value(QStringLiteral("filesystem")).toString().trimmed();
        QString title = name;
        if (!letter.isEmpty()) {
            title = letter + QStringLiteral(": ") + name;
        }
        out.push_back(QVariantMap{
            {QStringLiteral("volumeIndex"), volume_index},
            {QStringLiteral("letter"), letter},
            {QStringLiteral("name"), name},
            {QStringLiteral("title"), title},
            {QStringLiteral("size"), format.format_bytes(size)},
            {QStringLiteral("capacityBytes"), size},
            {QStringLiteral("fileSystem"), fs},
            {QStringLiteral("fs"), fs},
        });
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
    const auto started =
        coordinator_->begin_request(request_id, body, [this](const QByteArray& frame_body) {
            return handle_recovery_point_frame(frame_body);
        });
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
        if (!coordinator_->continue_request(request_id, next_id, next_body)) {
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
    repository_uuid_.clear();
    pending_recovery_points_.clear();
    requested_token_.reset();
    last_file_uuid_.clear();
    repository_loading_ = false;
    repository_configured_ = false;
    repository_request_id_.clear();
    recovery_points_.clear();
    repository_error_code_ = message_code;
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
    const bool had_state = recovery_point_layout_loading_ || !recovery_point_source_disks_.isEmpty() ||
                           !recovery_point_source_volumes_.isEmpty() ||
                           !recovery_point_layout_error_code_.isEmpty() ||
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

void ServiceClient::addRepositoryConnection(const QString& display_name, const QString& locator) {
    start_repository_input_command(kAddRepositoryConnectionRequestKind, display_name, locator);
}

void ServiceClient::importRepositoryConnection(const QString& display_name,
                                               const QString& locator) {
    start_repository_input_command(kImportRepositoryConnectionRequestKind, display_name, locator);
}

void ServiceClient::testRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kTestRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::setDefaultRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kSetDefaultRepositoryRequestKind, connection_id);
}

void ServiceClient::removeRepositoryConnection(const QString& connection_id) {
    start_repository_resource_command(kRemoveRepositoryConnectionRequestKind, connection_id);
}

void ServiceClient::start_repository_input_command(const int request_kind,
                                                   const QString& display_name,
                                                   const QString& locator) {
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
        display_name.trimmed(), locator.trimmed());
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body, [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        });
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
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    emit repositoryCommandChanged();
    const auto body = encode_repository_connection_resource_request(
        repository_command_request_id_, repository_command_idempotency_key_, request_kind,
        connection_id);
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body, [this](const QByteArray& frame_body) {
            return handle_repository_command_frame(frame_body);
        });
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
        const auto message_code = root.value(QStringLiteral("message_code")).toString();
        finish_repository_command_failure(
            message_code.isEmpty() ? QStringLiteral("service.request_failed") : message_code);
        refreshConnections();
        return RequestDisposition::kFinished;
    }
    CommandAck acknowledgement;
    if (!parse_command_ack_response(root, repository_command_kind_, acknowledgement)) {
        return RequestDisposition::kProtocolError;
    }
    if (repository_command_kind_ == kRemoveRepositoryConnectionRequestKind &&
        acknowledgement.resource_id == selected_repository_connection_id_) {
        selected_repository_connection_id_.clear();
        reset_repository();
    } else if (acknowledgement.has_resource_id) {
        selected_repository_connection_id_ = acknowledgement.resource_id;
    }
    reset_repository_command();
    refreshConnections();
    emit repositoryChanged();
    return RequestDisposition::kFinished;
}

void ServiceClient::finish_repository_command_failure(const QString& message_code) {
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_error_code_ = message_code;
    emit repositoryCommandChanged();
}

void ServiceClient::reset_repository_command() {
    repository_command_busy_ = false;
    repository_command_request_id_.clear();
    repository_command_idempotency_key_.clear();
    repository_command_kind_ = 0;
    repository_command_error_code_.clear();
    emit repositoryCommandChanged();
}

} // namespace aegra::desktop
