#include "aegra/format/manifest_codec.h"

#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>

namespace aegra::format {
namespace {

using Json = nlohmann::json;

[[nodiscard]] std::vector<std::uint8_t> to_octets(std::span<const std::byte> bytes) {
    std::vector<std::uint8_t> result(bytes.size());
    std::transform(bytes.begin(), bytes.end(), result.begin(),
                   [](std::byte value) { return std::to_integer<std::uint8_t>(value); });
    return result;
}

[[nodiscard]] std::vector<std::byte> to_bytes(const std::vector<std::uint8_t>& octets) {
    std::vector<std::byte> result(octets.size());
    std::transform(octets.begin(), octets.end(), result.begin(),
                   [](std::uint8_t value) { return static_cast<std::byte>(value); });
    return result;
}

[[nodiscard]] Json encode_binary(std::span<const std::byte> bytes) {
    return Json::binary(to_octets(bytes));
}

[[nodiscard]] std::vector<std::byte> decode_binary(const Json& value) {
    const auto& binary = value.get_binary();
    return to_bytes(std::vector<std::uint8_t>(binary.begin(), binary.end()));
}

[[nodiscard]] Json encode_partition(const Partition& partition) {
    return {{"partition_number", partition.partition_number},
            {"offset", partition.offset},
            {"size", partition.size},
            {"partition_style", partition.style},
            {"is_active", partition.is_active},
            {"mbr_type", partition.mbr_type},
            {"gpt_type_guid", partition.gpt_type_guid},
            {"gpt_name", partition.gpt_name},
            {"volume_label", partition.volume_label},
            {"filesystem", partition.filesystem},
            {"volume_guid", partition.volume_guid}};
}

[[nodiscard]] Partition decode_partition(const Json& value) {
    Partition result;
    value.at("partition_number").get_to(result.partition_number);
    value.at("offset").get_to(result.offset);
    value.at("size").get_to(result.size);
    result.style = static_cast<PartitionStyle>(value.at("partition_style").get<std::uint8_t>());
    value.at("is_active").get_to(result.is_active);
    value.at("mbr_type").get_to(result.mbr_type);
    value.at("gpt_type_guid").get_to(result.gpt_type_guid);
    value.at("gpt_name").get_to(result.gpt_name);
    value.at("volume_label").get_to(result.volume_label);
    value.at("filesystem").get_to(result.filesystem);
    value.at("volume_guid").get_to(result.volume_guid);
    return result;
}

[[nodiscard]] Json encode_raw_layout(const RawDiskLayout& layout) {
    return {{"mbr_sector", encode_binary(layout.mbr_sector)},
            {"gpt_primary_header", encode_binary(layout.gpt_primary_header)},
            {"gpt_partition_entries", encode_binary(layout.gpt_partition_entries)},
            {"gpt_backup_header", encode_binary(layout.gpt_backup_header)},
            {"gpt_backup_entries", encode_binary(layout.gpt_backup_entries)}};
}

[[nodiscard]] RawDiskLayout decode_raw_layout(const Json& value) {
    RawDiskLayout result;
    result.mbr_sector = decode_binary(value.at("mbr_sector"));
    result.gpt_primary_header = decode_binary(value.at("gpt_primary_header"));
    result.gpt_partition_entries = decode_binary(value.at("gpt_partition_entries"));
    result.gpt_backup_header = decode_binary(value.at("gpt_backup_header"));
    result.gpt_backup_entries = decode_binary(value.at("gpt_backup_entries"));
    return result;
}

[[nodiscard]] Json encode_disk(const Disk& disk) {
    Json partitions = Json::array();
    for (const auto& partition : disk.partitions) {
        partitions.push_back(encode_partition(partition));
    }
    return {{"disk_number", disk.disk_number},
            {"disk_size", disk.disk_size},
            {"bytes_per_sector", disk.bytes_per_sector},
            {"total_sectors", disk.total_sectors},
            {"partition_style", disk.partition_style},
            {"model", disk.model},
            {"serial", disk.serial},
            {"media_type", disk.media_type},
            {"partitions", std::move(partitions)},
            {"raw_layout", encode_raw_layout(disk.raw_layout)}};
}

[[nodiscard]] Disk decode_disk(const Json& value) {
    Disk result;
    value.at("disk_number").get_to(result.disk_number);
    value.at("disk_size").get_to(result.disk_size);
    value.at("bytes_per_sector").get_to(result.bytes_per_sector);
    value.at("total_sectors").get_to(result.total_sectors);
    result.partition_style =
        static_cast<PartitionStyle>(value.at("partition_style").get<std::uint8_t>());
    value.at("model").get_to(result.model);
    value.at("serial").get_to(result.serial);
    value.at("media_type").get_to(result.media_type);
    for (const auto& partition : value.at("partitions")) {
        result.partitions.push_back(decode_partition(partition));
    }
    result.raw_layout = decode_raw_layout(value.at("raw_layout"));
    return result;
}

[[nodiscard]] Json encode_extent(const VolumeExtent& extent) {
    return {{"disk_number", extent.disk_number},
            {"partition_number", extent.partition_number},
            {"physical_offset", extent.physical_offset},
            {"volume_offset", extent.volume_offset},
            {"length", extent.length},
            {"extent_role", extent.extent_role}};
}

[[nodiscard]] VolumeExtent decode_extent(const Json& value) {
    VolumeExtent result;
    value.at("disk_number").get_to(result.disk_number);
    value.at("partition_number").get_to(result.partition_number);
    value.at("physical_offset").get_to(result.physical_offset);
    value.at("volume_offset").get_to(result.volume_offset);
    value.at("length").get_to(result.length);
    value.at("extent_role").get_to(result.extent_role);
    return result;
}

[[nodiscard]] Json encode_volume(const Volume& volume) {
    Json extents = Json::array();
    for (const auto& extent : volume.extents) {
        extents.push_back(encode_extent(extent));
    }
    return {{"volume_index", volume.volume_index},
            {"volume_id", volume.volume_id},
            {"volume_guid", volume.volume_guid},
            {"mount_points", volume.mount_points},
            {"filesystem", volume.filesystem},
            {"label", volume.label},
            {"total_size", volume.total_size},
            {"free_size", volume.free_size},
            {"free_size_known", volume.free_size_known},
            {"cluster_size", volume.cluster_size},
            {"vss_required", volume.vss_required},
            {"vss_used", volume.vss_used},
            {"consistency_level", volume.consistency_level},
            {"extents", std::move(extents)}};
}

[[nodiscard]] Volume decode_volume(const Json& value) {
    Volume result;
    value.at("volume_index").get_to(result.volume_index);
    value.at("volume_id").get_to(result.volume_id);
    value.at("volume_guid").get_to(result.volume_guid);
    value.at("mount_points").get_to(result.mount_points);
    value.at("filesystem").get_to(result.filesystem);
    value.at("label").get_to(result.label);
    value.at("total_size").get_to(result.total_size);
    value.at("free_size").get_to(result.free_size);
    value.at("free_size_known").get_to(result.free_size_known);
    if (result.free_size_known) {
        if (result.free_size > result.total_size) {
            throw std::invalid_argument("volume free_size exceeds total_size");
        }
    } else if (result.free_size != 0) {
        throw std::invalid_argument("volume free_size must be 0 when free_size_known is false");
    }
    value.at("cluster_size").get_to(result.cluster_size);
    value.at("vss_required").get_to(result.vss_required);
    value.at("vss_used").get_to(result.vss_used);
    result.consistency_level =
        static_cast<ConsistencyLevel>(value.at("consistency_level").get<std::uint8_t>());
    for (const auto& extent : value.at("extents")) {
        result.extents.push_back(decode_extent(extent));
    }
    return result;
}

[[nodiscard]] Json encode_system(const SystemInfo& system) {
    return {{"host", {{"hostname", system.hostname}, {"machine_guid", system.machine_guid}}},
            {"os",
             {{"name", system.os_name},
              {"version", system.os_version},
              {"architecture", system.os_architecture}}},
            {"collection_time_utc", system.collection_time_utc}};
}

[[nodiscard]] SystemInfo decode_system(const Json& value) {
    SystemInfo result;
    const auto& host = value.at("host");
    const auto& os = value.at("os");
    host.at("hostname").get_to(result.hostname);
    host.at("machine_guid").get_to(result.machine_guid);
    os.at("name").get_to(result.os_name);
    os.at("version").get_to(result.os_version);
    os.at("architecture").get_to(result.os_architecture);
    value.at("collection_time_utc").get_to(result.collection_time_utc);
    return result;
}

[[nodiscard]] Json encode_job(const BackupJob& job) {
    return {{"backup_type", job.backup_type},
            {"created_utc", job.created_utc},
            {"application_version", job.application_version},
            {"description", job.description}};
}

[[nodiscard]] BackupJob decode_job(const Json& value) {
    BackupJob result;
    result.backup_type = static_cast<BackupType>(value.at("backup_type").get<std::uint8_t>());
    value.at("created_utc").get_to(result.created_utc);
    value.at("application_version").get_to(result.application_version);
    value.at("description").get_to(result.description);
    return result;
}

[[nodiscard]] Json encode_extensions(const Manifest& manifest) {
    Json result = Json::object();
    for (const auto& extension : manifest.extensions) {
        result[extension.key] = encode_binary(extension.payload);
    }
    return result;
}

[[nodiscard]] Json encode_file_set_baseline(const FileSetBaseline& baseline) {
    return {
        {"fingerprint_algorithm", baseline.fingerprint_algorithm},
        {"selection_fingerprint", encode_binary(baseline.selection_fingerprint)},
        {"change_detection_method", static_cast<std::uint8_t>(baseline.change_detection_method)}};
}

[[nodiscard]] FileSetBaseline decode_file_set_baseline(const Json& value) {
    if (!value.is_object() || value.size() != 3 || !value.contains("fingerprint_algorithm") ||
        !value.contains("selection_fingerprint") || !value.contains("change_detection_method")) {
        throw std::invalid_argument("file_set_baseline keys are invalid");
    }
    FileSetBaseline baseline;
    baseline.fingerprint_algorithm = value.at("fingerprint_algorithm").get<std::uint8_t>();
    const auto digest = decode_binary(value.at("selection_fingerprint"));
    if (digest.size() == baseline.selection_fingerprint.size()) {
        std::copy(digest.begin(), digest.end(), baseline.selection_fingerprint.begin());
    }
    baseline.change_detection_method = static_cast<contracts::FileChangeDetectionMethod>(
        value.at("change_detection_method").get<std::uint8_t>());
    return baseline;
}

[[nodiscard]] Manifest decode_root(const Json& root) {
    Manifest result;
    root.at("schema_version").get_to(result.schema_version);
    if (root.contains("content_kind")) {
        result.content_kind = root.at("content_kind").get<std::uint8_t>();
    }
    for (const auto& disk : root.at("disks")) {
        result.disks.push_back(decode_disk(disk));
    }
    result.system = decode_system(root.at("system"));
    result.backup_job = decode_job(root.at("backup_job"));
    for (const auto& volume : root.at("volumes")) {
        result.volumes.push_back(decode_volume(volume));
    }
    if (root.contains("file_set_baseline")) {
        result.file_set_baseline = decode_file_set_baseline(root.at("file_set_baseline"));
    }
    for (const auto& [key, value] : root.at("extensions").items()) {
        result.extensions.push_back({key, decode_binary(value)});
    }
    return result;
}

[[nodiscard]] Json encode_root(const Manifest& manifest) {
    Json disks = Json::array();
    Json volumes = Json::array();
    for (const auto& disk : manifest.disks) {
        disks.push_back(encode_disk(disk));
    }
    for (const auto& volume : manifest.volumes) {
        volumes.push_back(encode_volume(volume));
    }
    Json root = {{"schema_version", manifest.schema_version},
                 {"content_kind", manifest.content_kind},
                 {"disks", std::move(disks)},
                 {"system", encode_system(manifest.system)},
                 {"backup_job", encode_job(manifest.backup_job)},
                 {"volumes", std::move(volumes)},
                 {"extensions", encode_extensions(manifest)}};
    if (manifest.content_kind == kManifestContentKindFileSet) {
        root["file_set_baseline"] = encode_file_set_baseline(manifest.file_set_baseline);
    }
    return root;
}

[[nodiscard]] base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

} // namespace

base::Result<std::vector<std::byte>> encode_manifest_cbor(const Manifest& manifest) {
    auto validation = validate_manifest(manifest);
    if (!validation) {
        return base::Result<std::vector<std::byte>>::failure(validation.error());
    }
    try {
        return base::Result<std::vector<std::byte>>::success(
            to_bytes(Json::to_cbor(encode_root(manifest))));
    } catch (const std::exception& exception) {
        return base::Result<std::vector<std::byte>>::failure(
            corrupt(std::string("failed to encode manifest CBOR: ") + exception.what()));
    }
}

base::Result<Manifest> decode_manifest_cbor(std::span<const std::byte> encoded) {
    try {
        auto manifest = decode_root(Json::from_cbor(to_octets(encoded), true, true));
        auto validation = validate_manifest(manifest);
        if (!validation) {
            return base::Result<Manifest>::failure(validation.error());
        }
        return base::Result<Manifest>::success(std::move(manifest));
    } catch (const std::exception& exception) {
        return base::Result<Manifest>::failure(
            corrupt(std::string("failed to decode manifest CBOR: ") + exception.what()));
    }
}

} // namespace aegra::format
