#include "recovery_point_layout_service.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
#include "aegra/personal_repository/catalog_scanner.h"
#include "aegra/ports/repository_storage.h"

#include <filesystem>
#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kDefaultLocalArchivePassword = "aegra-local";

[[nodiscard]] base::Result<std::filesystem::path> path_from_utf8(const std::string_view value) {
    try {
        const auto* begin = reinterpret_cast<const char8_t*>(value.data());
        std::filesystem::path path(std::u8string(begin, begin + value.size()));
        if (!path.is_absolute()) {
            return base::Result<std::filesystem::path>::failure(
                {base::ErrorCode::kInvalidArgument, "repository locator must be absolute"});
        }
        return base::Result<std::filesystem::path>::success(std::move(path));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "repository locator is invalid UTF-8"});
    }
}

[[nodiscard]] base::Result<std::filesystem::path>
resolve_archive_path(const std::string& locator, const std::string& archive_main_key) {
    auto root = path_from_utf8(locator);
    if (!root) {
        return base::Result<std::filesystem::path>::failure(root.error());
    }
    if (!archive_main_key.starts_with("archives/") ||
        archive_main_key.find('\\') != std::string::npos ||
        archive_main_key.find(':') != std::string::npos ||
        archive_main_key.find("..") != std::string::npos) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is outside the archive root"});
    }
    std::filesystem::path relative;
    try {
        relative = std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(archive_main_key.data()), archive_main_key.size()));
    } catch (const std::exception&) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kInvalidArgument, "archive key is invalid"});
    }
    std::error_code error_code;
    const auto canonical_root = std::filesystem::weakly_canonical(root.value(), error_code);
    if (error_code) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kIoFailure, "repository root cannot be resolved"});
    }
    const auto canonical_archive =
        std::filesystem::weakly_canonical(canonical_root / relative, error_code);
    if (error_code) {
        return base::Result<std::filesystem::path>::failure(
            {base::ErrorCode::kIoFailure, "archive path cannot be resolved"});
    }
    return base::Result<std::filesystem::path>::success(canonical_archive);
}

[[nodiscard]] std::string mount_letter(const format::Volume& volume) {
    for (const auto& mount : volume.mount_points) {
        if (mount.size() >= 2 && mount[1] == ':') {
            return mount.substr(0, 2);
        }
    }
    return {};
}

[[nodiscard]] base::Result<personal_repository::CatalogEntry>
find_catalog_entry(ports::IRepositoryStorageAccess& storage, const std::string_view recovery_point_id,
                   const base::CancellationToken cancellation) {
    personal_repository::RepositoryCatalogScanner scanner(storage.reader(), storage.enumerator(),
                                                          {});
    std::optional<std::string> token;
    for (;;) {
        personal_repository::CatalogScanRequest request;
        request.continuation_token = token;
        request.maximum_results = 100;
        auto page = scanner.scan(request, cancellation);
        if (!page) {
            return base::Result<personal_repository::CatalogEntry>::failure(page.error());
        }
        for (const auto& point : page.value().recovery_points) {
            if (point.entry.file_uuid == recovery_point_id) {
                return base::Result<personal_repository::CatalogEntry>::success(point.entry);
            }
        }
        if (!page.value().continuation_token) {
            break;
        }
        token = std::move(page.value().continuation_token);
    }
    return base::Result<personal_repository::CatalogEntry>::failure(
        {base::ErrorCode::kNotFound, "recovery point was not found in the catalog"});
}

} // namespace

base::Result<contracts::RecoveryPointLayout>
load_recovery_point_layout(ports::IControlPlaneDatabase& control_plane,
                           ports::IRepositoryStorageFactory& storage_factory,
                           const contracts::RecoveryPointRef& reference,
                           const base::CancellationToken cancellation) {
    auto valid = contracts::validate_recovery_point_ref(reference);
    if (!valid) {
        return base::Result<contracts::RecoveryPointLayout>::failure(valid.error());
    }
    auto connection =
        control_plane.get_repository_connection(reference.repository_connection_id, cancellation);
    if (!connection) {
        return base::Result<contracts::RecoveryPointLayout>::failure(connection.error());
    }
    if (!connection.value() ||
        connection.value()->state != contracts::RepositoryConnectionState::kAvailable) {
        return base::Result<contracts::RecoveryPointLayout>::failure(
            {base::ErrorCode::kConflict, "repository connection is unavailable"});
    }
    auto storage = storage_factory.open(connection.value()->locator, cancellation);
    if (!storage) {
        return base::Result<contracts::RecoveryPointLayout>::failure(storage.error());
    }
    auto entry = find_catalog_entry(*storage.value(), reference.recovery_point_id, cancellation);
    if (!entry) {
        return base::Result<contracts::RecoveryPointLayout>::failure(entry.error());
    }
    auto archive_path =
        resolve_archive_path(connection.value()->locator, entry.value().archive_main_key);
    if (!archive_path) {
        return base::Result<contracts::RecoveryPointLayout>::failure(archive_path.error());
    }
    // Personal local archives use the same fixed password material as the worker when no
    // repository SecretRef is mapped (aegra-local).
    adapters::personal_archive::ArchiveOpenRequest open_request;
    open_request.source = std::move(archive_path).value();
    open_request.password = kDefaultLocalArchivePassword;
    auto reader = adapters::personal_archive::PersonalArchiveReader::open(open_request);
    if (!reader) {
        return base::Result<contracts::RecoveryPointLayout>::failure(reader.error());
    }
    const auto& manifest = reader.value()->manifest();
    if (manifest.disks.empty()) {
        return base::Result<contracts::RecoveryPointLayout>::failure(
            {base::ErrorCode::kConflict,
             "recovery point layout is incomplete; re-run backup after disk layout support"});
    }
    contracts::RecoveryPointLayout layout;
    layout.repository_connection_id = reference.repository_connection_id;
    layout.recovery_point_id = reference.recovery_point_id;
    layout.disks.reserve(manifest.disks.size());
    for (const auto& disk : manifest.disks) {
        contracts::RecoveryPointSourceDisk item;
        item.disk_number = disk.disk_number;
        item.disk_size_bytes = disk.disk_size;
        switch (disk.partition_style) {
        case format::PartitionStyle::kMbr:
            item.partition_style = "mbr";
            break;
        case format::PartitionStyle::kGpt:
            item.partition_style = "gpt";
            break;
        default:
            item.partition_style = "raw";
            break;
        }
        item.model = disk.model;
        item.media_type = disk.media_type;
        item.partitions.reserve(disk.partitions.size());
        for (const auto& partition : disk.partitions) {
            contracts::RecoveryPointSourcePartition part;
            part.partition_number = partition.partition_number;
            part.offset_bytes = partition.offset;
            part.size_bytes = partition.size;
            part.is_active = partition.is_active;
            part.mbr_type = partition.mbr_type;
            part.gpt_type_guid = partition.gpt_type_guid;
            part.gpt_name = partition.gpt_name;
            part.volume_label = partition.volume_label;
            part.filesystem = partition.filesystem;
            item.partitions.push_back(std::move(part));
        }
        layout.disks.push_back(std::move(item));
    }
    layout.volumes.reserve(manifest.volumes.size());
    for (const auto& volume : manifest.volumes) {
        contracts::RecoveryPointSourceVolume item;
        item.volume_index = volume.volume_index;
        item.letter = mount_letter(volume);
        item.label = volume.label;
        item.filesystem = volume.filesystem;
        item.total_size_bytes = volume.total_size;
        item.extents.reserve(volume.extents.size());
        for (const auto& extent : volume.extents) {
            contracts::RecoveryPointSourceExtent mapped;
            mapped.disk_number = extent.disk_number;
            mapped.partition_number = extent.partition_number;
            mapped.physical_offset = extent.physical_offset;
            mapped.volume_offset = extent.volume_offset;
            mapped.length = extent.length;
            item.extents.push_back(std::move(mapped));
        }
        layout.volumes.push_back(std::move(item));
    }
    auto valid_layout = contracts::validate_recovery_point_layout(layout);
    if (!valid_layout) {
        return base::Result<contracts::RecoveryPointLayout>::failure(valid_layout.error());
    }
    return base::Result<contracts::RecoveryPointLayout>::success(std::move(layout));
}

} // namespace aegra::apps::service
