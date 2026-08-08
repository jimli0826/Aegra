#include "aegra/apps/service/backup_catalog_registrar.h"

#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/base/uuid.h"
#include "aegra/format/personal_archive.h"
#include "aegra/format/personal_archive_sidecar.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/repository_storage.h"

#include <array>
#include <cstdio>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

namespace archive = format::personal_archive;

[[nodiscard]] base::Error registration_error(const base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_object(ports::IObjectReader& reader, const std::string_view key,
            const std::uint64_t maximum_size, const base::CancellationToken cancellation) {
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes) {
        return base::Result<std::vector<std::byte>>::failure(attributes.error());
    }
    if (attributes.value().size_bytes > maximum_size ||
        attributes.value().size_bytes > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<std::vector<std::byte>>::failure(
            registration_error(base::ErrorCode::kCorruptData, "repository object is too large"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(attributes.value().size_bytes));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto read = reader.read_range(key, offset, std::span(bytes).subspan(offset), cancellation);
        if (!read || read.value() == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                !read ? read.error()
                      : registration_error(base::ErrorCode::kIoFailure,
                                           "repository object was short read"));
        }
        offset += read.value();
    }
    return base::Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] base::Result<personal_repository::RepositoryDescriptor>
read_descriptor(ports::IObjectReader& reader, const base::CancellationToken cancellation) {
    auto bytes = read_object(reader, "aegra.repository", 4ULL * 1024ULL * 1024ULL, cancellation);
    if (!bytes) {
        return base::Result<personal_repository::RepositoryDescriptor>::failure(bytes.error());
    }
    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                       bytes.value().size());
    return personal_repository::decode_repository_descriptor_json(text);
}

[[nodiscard]] std::string part_key(const std::string_view main_key, const std::uint32_t index) {
    if (index == 0) {
        return std::string(main_key);
    }
    std::array<char, 8> suffix{};
    std::snprintf(suffix.data(), suffix.size(), ".%03u", index);
    return std::string(main_key) + suffix.data();
}

[[nodiscard]] base::Result<archive::BackupHeader>
read_archive_header(ports::IObjectReader& reader, const std::string_view key,
                    const base::CancellationToken cancellation) {
    std::array<std::byte, archive::kBackupHeaderSize> bytes{};
    auto read = reader.read_range(key, 0, bytes, cancellation);
    if (!read) {
        return base::Result<archive::BackupHeader>::failure(read.error());
    }
    if (read.value() != bytes.size()) {
        return base::Result<archive::BackupHeader>::failure(
            registration_error(base::ErrorCode::kCorruptData, "archive header is truncated"));
    }
    return archive::decode_backup_header(bytes);
}

struct ArchiveInspection final {
    std::uint32_t split_part_count{1};
    bool has_sidecar{false};
    std::uint64_t stored_size_bytes{0};
};

[[nodiscard]] std::uint32_t backup_type_flag(const contracts::BackupType type) noexcept {
    switch (type) {
    case contracts::BackupType::kFull:
        return archive::kBackupFlagFull;
    case contracts::BackupType::kIncremental:
        return archive::kBackupFlagIncremental;
    case contracts::BackupType::kDifferential:
        return archive::kBackupFlagDifferential;
    }
    return 0;
}

[[nodiscard]] base::Result<void>
validate_part(const archive::BackupHeader& header, const base::UuidBytes& file_uuid,
              const base::UuidBytes& set_uuid, const std::uint32_t index,
              const std::uint32_t split_count) {
    if (header.file_uuid != file_uuid || header.backup_set_uuid != set_uuid ||
        header.split_part_index != index || header.split_part_count != split_count) {
        return base::Result<void>::failure(
            registration_error(base::ErrorCode::kCorruptData, "archive part identity mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_final_footer(ports::IObjectReader& reader, const std::string_view key,
                      const std::uint64_t size_bytes,
                      const base::CancellationToken cancellation) {
    if (size_bytes < archive::kBackupFooterSize) {
        return base::Result<void>::failure(
            registration_error(base::ErrorCode::kCorruptData, "archive footer is missing"));
    }
    std::array<std::byte, archive::kBackupFooterSize> bytes{};
    auto read = reader.read_range(key, size_bytes - bytes.size(), bytes, cancellation);
    if (!read || read.value() != bytes.size()) {
        return base::Result<void>::failure(
            !read ? read.error()
                  : registration_error(base::ErrorCode::kCorruptData,
                                       "archive footer is truncated"));
    }
    auto footer = archive::decode_backup_footer(bytes);
    if (!footer || footer.value().part_file_size != size_bytes) {
        return base::Result<void>::failure(
            !footer ? footer.error()
                    : registration_error(base::ErrorCode::kCorruptData,
                                         "archive footer size mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<bool>
inspect_sidecar(ports::IObjectReader& reader, const std::string& main_key,
                const base::UuidBytes& file_uuid, const base::CancellationToken cancellation) {
    const auto key = main_key + ".bhx";
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
        return base::Result<bool>::success(false);
    }
    if (!attributes) {
        return base::Result<bool>::failure(attributes.error());
    }
    std::array<std::byte, archive::kSidecarHeaderSize> bytes{};
    auto read = reader.read_range(key, 0, bytes, cancellation);
    if (!read || read.value() != bytes.size()) {
        return base::Result<bool>::failure(
            !read ? read.error()
                  : registration_error(base::ErrorCode::kCorruptData,
                                       "archive sidecar header is truncated"));
    }
    auto header = archive::decode_sidecar_header(bytes);
    if (!header || header.value().file_uuid != file_uuid) {
        return base::Result<bool>::failure(
            !header ? header.error()
                    : registration_error(base::ErrorCode::kCorruptData,
                                         "archive sidecar identity mismatch"));
    }
    return base::Result<bool>::success(true);
}

[[nodiscard]] base::Result<ArchiveInspection>
inspect_archive(ports::IObjectReader& reader, const std::string& main_key,
                const std::string_view file_uuid_text, const std::string_view set_uuid_text,
                const contracts::BackupType backup_type,
                const base::CancellationToken cancellation) {
    auto file_uuid = base::parse_uuid(file_uuid_text);
    auto set_uuid = base::parse_uuid(set_uuid_text);
    auto primary = read_archive_header(reader, main_key, cancellation);
    if (!file_uuid || !set_uuid || !primary) {
        const auto& error = !file_uuid ? file_uuid.error() : !set_uuid ? set_uuid.error()
                                                                  : primary.error();
        return base::Result<ArchiveInspection>::failure(error);
    }
    const auto type_bits = primary.value().flags &
                           (archive::kBackupFlagFull | archive::kBackupFlagIncremental |
                            archive::kBackupFlagDifferential);
    if (type_bits != backup_type_flag(backup_type)) {
        return base::Result<ArchiveInspection>::failure(registration_error(
            base::ErrorCode::kCorruptData, "archive backup type does not match the job"));
    }
    const bool split = (primary.value().flags & archive::kBackupFlagSplit) != 0;
    const auto declared_count = primary.value().split_part_count;
    if (declared_count > personal_repository::kMaximumSplitPartCount) {
        return base::Result<ArchiveInspection>::failure(registration_error(
            base::ErrorCode::kCorruptData, "archive split part count exceeds the limit"));
    }
    const auto maximum_count = declared_count != 0 ? declared_count
                                                    : personal_repository::kMaximumSplitPartCount;
    ArchiveInspection result;
    std::string final_key;
    std::uint64_t final_size = 0;
    for (std::uint32_t index = 0; index < maximum_count; ++index) {
        const auto key = part_key(main_key, index);
        auto attributes = reader.get_attributes(key, cancellation);
        if (!attributes && attributes.error().code == base::ErrorCode::kNotFound && index > 0 &&
            split && declared_count == 0) {
            break;
        }
        auto header = read_archive_header(reader, key, cancellation);
        if (!attributes || !header) {
            return base::Result<ArchiveInspection>::failure(!attributes ? attributes.error()
                                                                        : header.error());
        }
        auto valid = validate_part(header.value(), file_uuid.value(), set_uuid.value(), index,
                                   declared_count);
        if (!valid || result.stored_size_bytes >
                          (std::numeric_limits<std::uint64_t>::max)() - attributes.value().size_bytes) {
            return base::Result<ArchiveInspection>::failure(
                !valid ? valid.error()
                       : registration_error(base::ErrorCode::kCorruptData,
                                            "archive size overflow"));
        }
        result.stored_size_bytes += attributes.value().size_bytes;
        result.split_part_count = index + 1;
        final_key = key;
        final_size = attributes.value().size_bytes;
        if (!split) {
            break;
        }
    }
    if (split && declared_count == 0 &&
        result.split_part_count == personal_repository::kMaximumSplitPartCount) {
        auto extra = reader.get_attributes(part_key(main_key, result.split_part_count), cancellation);
        if (extra || extra.error().code != base::ErrorCode::kNotFound) {
            return base::Result<ArchiveInspection>::failure(
                extra ? registration_error(base::ErrorCode::kCorruptData,
                                           "archive split part count exceeds the limit")
                      : extra.error());
        }
    }
    auto footer = validate_final_footer(reader, final_key, final_size, cancellation);
    if (!footer) {
        return base::Result<ArchiveInspection>::failure(footer.error());
    }
    auto sidecar = inspect_sidecar(reader, main_key, file_uuid.value(), cancellation);
    if (!sidecar) {
        return base::Result<ArchiveInspection>::failure(sidecar.error());
    }
    result.has_sidecar = sidecar.value();
    return base::Result<ArchiveInspection>::success(result);
}

[[nodiscard]] base::Result<std::optional<personal_repository::CatalogEntry>>
read_catalog_entry(ports::IObjectReader& reader, const std::string_view key,
                   const base::CancellationToken cancellation) {
    auto bytes = read_object(reader, key, 4ULL * 1024ULL * 1024ULL, cancellation);
    if (!bytes && bytes.error().code == base::ErrorCode::kNotFound) {
        return base::Result<std::optional<personal_repository::CatalogEntry>>::success(std::nullopt);
    }
    if (!bytes) {
        return base::Result<std::optional<personal_repository::CatalogEntry>>::failure(bytes.error());
    }
    const auto text = std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                       bytes.value().size());
    auto decoded = personal_repository::decode_catalog_entry_json(text);
    return decoded
               ? base::Result<std::optional<personal_repository::CatalogEntry>>::success(
                     std::move(decoded).value())
               : base::Result<std::optional<personal_repository::CatalogEntry>>::failure(
                     decoded.error());
}

[[nodiscard]] base::Result<void>
publish_entry(ports::IRepositoryStorageAccess& storage,
              const personal_repository::RepositoryDescriptor& descriptor,
              const personal_repository::CatalogEntry& entry, const std::string_view job_id,
              const base::CancellationToken cancellation) {
    auto encoded = personal_repository::encode_catalog_entry_json(entry);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    const auto staging = descriptor.staging_prefix + "/" + entry.file_uuid + "/" +
                         std::string(job_id) + ".entry";
    const auto destination = descriptor.catalog_prefix + "/" + entry.file_uuid + ".entry";
    auto session = storage.writer().begin_staged_write(staging, cancellation);
    if (!session) {
        return base::Result<void>::failure(session.error());
    }
    const auto bytes = std::as_bytes(std::span(encoded.value().data(), encoded.value().size()));
    auto written = session.value()->write(bytes, cancellation);
    auto completed = written ? session.value()->complete(cancellation)
                             : base::Result<void>::failure(written.error());
    if (!completed) {
        session.value()->abort();
        return completed;
    }
    auto published = storage.publisher().publish(
        {staging, destination, ports::PublishCondition::kCreateOnly, std::nullopt}, cancellation);
    if (published) {
        return base::Result<void>::success();
    }
    auto existing = read_catalog_entry(storage.reader(), destination, cancellation);
    if (existing && existing.value() && *existing.value() == entry) {
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(!existing ? existing.error() : published.error());
}

[[nodiscard]] format::BackupType catalog_backup_type(const contracts::BackupType type) noexcept {
    switch (type) {
    case contracts::BackupType::kIncremental:
        return format::BackupType::kIncremental;
    case contracts::BackupType::kDifferential:
        return format::BackupType::kDifferential;
    case contracts::BackupType::kFull:
        return format::BackupType::kFull;
    }
    return format::BackupType::kFull;
}

} // namespace

BackupCatalogRegistrar::BackupCatalogRegistrar(
    ports::IControlPlaneDatabase& control_plane,
    ports::IRepositoryStorageFactory& storage_factory) noexcept
    : control_plane_(control_plane), storage_factory_(storage_factory) {}

base::Result<void>
BackupCatalogRegistrar::publish(const WorkerJobRequest& request,
                                const contracts::WorkerResponse& response,
                                const base::CancellationToken cancellation) const {
    if (request.worker_request.operation != contracts::JobOperation::kBackup ||
        !request.worker_request.backup || !request.backup_archive_key || !response.task_result ||
        (response.task_result->outcome != contracts::TaskOutcome::kSucceeded &&
         response.task_result->outcome != contracts::TaskOutcome::kSucceededWithWarning)) {
        return base::Result<void>::success();
    }
    auto repository = control_plane_.get_repository_connection(request.repository_connection_id,
                                                               cancellation);
    if (!repository || !repository.value()) {
        return base::Result<void>::failure(
            !repository ? repository.error()
                        : registration_error(base::ErrorCode::kNotFound,
                                             "repository connection was not found"));
    }
    auto storage = storage_factory_.open(repository.value()->locator, cancellation);
    if (!storage) {
        return base::Result<void>::failure(storage.error());
    }
    auto descriptor = read_descriptor(storage.value()->reader(), cancellation);
    if (!descriptor) {
        return base::Result<void>::failure(descriptor.error());
    }
    const auto& backup = *request.worker_request.backup;
    const bool is_file_set =
        request.worker_request.content_kind == contracts::ContentKind::kFileSet;
    // file_set may demote Incremental→Full; Catalog must publish the *effective* type/parent.
    contracts::BackupType effective_type = backup.type;
    std::optional<std::string> effective_parent;
    if (is_file_set && response.task_result->effective_backup_type) {
        effective_type =
            static_cast<contracts::BackupType>(*response.task_result->effective_backup_type);
        if (response.task_result->effective_parent_uuid &&
            !response.task_result->effective_parent_uuid->empty()) {
            effective_parent = *response.task_result->effective_parent_uuid;
        }
    } else if (!is_file_set && backup.type == contracts::BackupType::kIncremental) {
        effective_parent = request.parent_recovery_point_id;
    }
    std::string set_uuid = backup.backup_set_uuid;
    if (effective_type == contracts::BackupType::kIncremental) {
        if (!effective_parent) {
            return base::Result<void>::failure(registration_error(
                base::ErrorCode::kInternal, "incremental backup is missing parent identity"));
        }
        // volume_set Job leaves backup_set_uuid empty; inherit from the parent catalog entry.
        if (set_uuid.empty()) {
            auto parent = read_catalog_entry(
                storage.value()->reader(),
                descriptor.value().catalog_prefix + "/" + *effective_parent + ".entry",
                cancellation);
            if (!parent || !parent.value()) {
                return base::Result<void>::failure(
                    !parent ? parent.error()
                            : registration_error(base::ErrorCode::kNotFound,
                                                 "parent catalog entry was not found"));
            }
            set_uuid = parent.value()->backup_set_uuid;
        }
    }
    auto archive = inspect_archive(storage.value()->reader(), *request.backup_archive_key,
                                   backup.file_uuid, set_uuid, effective_type, cancellation);
    if (!archive) {
        return base::Result<void>::failure(archive.error());
    }
    personal_repository::CatalogEntry entry;
    entry.repository_uuid = descriptor.value().repository_uuid;
    entry.file_uuid = backup.file_uuid;
    entry.backup_set_uuid = std::move(set_uuid);
    entry.parent_uuid = std::move(effective_parent);
    entry.backup_type = catalog_backup_type(effective_type);
    entry.content_kind =
        std::string(is_file_set ? personal_repository::kCatalogContentKindFileSet
                                : personal_repository::kCatalogContentKindVolumeSet);
    entry.archive_main_key = *request.backup_archive_key;
    entry.split_part_count = archive.value().split_part_count;
    entry.has_sidecar = is_file_set ? false : archive.value().has_sidecar;
    entry.format_version = personal_repository::kPersonalArchiveFormatVersion;
    entry.created_utc_ms = static_cast<std::uint64_t>(backup.created_utc_ms);
    entry.logical_size_bytes = response.task_result->logical_bytes;
    entry.stored_size_bytes = archive.value().stored_size_bytes;
    if (is_file_set) {
        entry.source_count =
            static_cast<std::uint32_t>(request.worker_request.file_source_refs.size());
        entry.source_volume_ids.clear();
        entry.file_entry_count = response.task_result->entry_count;
        entry.file_stream_count = response.task_result->stream_count;
        // Hex fingerprint for parent match; USN checkpoints live in authenticated Manifest.
        if (request.worker_request.backup && request.worker_request.backup->selection_fingerprint) {
            const auto& digest = request.worker_request.backup->selection_fingerprint->digest;
            entry.file_selection_fingerprint.resize(digest.size() * 2);
            static constexpr char kHex[] = "0123456789abcdef";
            for (std::size_t index = 0; index < digest.size(); ++index) {
                const auto value = static_cast<unsigned char>(digest[index]);
                entry.file_selection_fingerprint[index * 2] = kHex[value >> 4U];
                entry.file_selection_fingerprint[index * 2 + 1] = kHex[value & 0x0FU];
            }
        }
        // Successful Worker publish carries authenticated selection fingerprint + USN baseline.
        entry.file_baseline_available = !entry.file_selection_fingerprint.empty();
    } else {
        entry.source_count =
            static_cast<std::uint32_t>(request.worker_request.source_refs.size());
        // Ordered stable Volume GUID paths used by the Worker; parent selection matches these.
        entry.source_volume_ids = request.worker_request.source_refs;
        entry.file_entry_count = 0;
        entry.file_stream_count = 0;
        entry.file_selection_fingerprint.clear();
        entry.file_baseline_available = false;
    }
    auto published = publish_entry(*storage.value(), descriptor.value(), entry,
                                   request.worker_request.job_id, cancellation);
    if (!published) {
        return published;
    }
    // Advance schedule tip only after Catalog is durable; next Incremental uses this file_uuid.
    if (request.schedule_id.empty()) {
        return base::Result<void>::success();
    }
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<void>::failure(unit.error());
    }
    auto schedule = unit.value()->schedules().get(request.schedule_id, cancellation);
    if (!schedule) {
        unit.value()->rollback();
        return base::Result<void>::failure(schedule.error());
    }
    if (!schedule.value()) {
        unit.value()->rollback();
        // Schedule may have been deleted after job start; Catalog publish still succeeded.
        return base::Result<void>::success();
    }
    auto record = std::move(*schedule.value());
    record.last_recovery_point_id = entry.file_uuid;
    auto upserted = unit.value()->schedules().upsert(record, cancellation);
    if (!upserted) {
        unit.value()->rollback();
        return base::Result<void>::failure(upserted.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<void>::success()
                     : base::Result<void>::failure(committed.error());
}

} // namespace aegra::apps::service
