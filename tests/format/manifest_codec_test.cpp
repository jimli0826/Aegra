#include "aegra/format/manifest_codec.h"

#include "aegra/base/error.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

aegra::format::Manifest make_manifest() {
    aegra::format::Manifest manifest;
    manifest.backup_job.created_utc = "2026-08-01T08:00:00Z";
    manifest.backup_job.application_version = "0.1.0";
    manifest.system.hostname = "backup-host";
    manifest.system.os_name = "Windows";
    manifest.system.os_architecture = "x64";

    aegra::format::Disk disk;
    disk.disk_number = 0;
    disk.disk_size = 4096;
    disk.bytes_per_sector = 512;
    disk.total_sectors = 8;
    disk.partition_style = aegra::format::PartitionStyle::kGpt;
    disk.partitions.push_back(
        {1, 512, 3584, aegra::format::PartitionStyle::kGpt, false, "Data", "NTFS", "volume-guid"});
    disk.raw_layout.gpt_primary_header = {std::byte{0x45}, std::byte{0x46}};
    manifest.disks.push_back(std::move(disk));

    aegra::format::Volume volume;
    volume.volume_id = "volume-guid";
    volume.volume_guid = "volume-guid";
    volume.total_size = 3584;
    volume.cluster_size = 4096;
    volume.consistency_level = aegra::format::ConsistencyLevel::kFilesystem;
    volume.extents.push_back({0, 1, 512, 0, 3584, "basic"});
    manifest.volumes.push_back(std::move(volume));
    manifest.extensions.push_back({"test.vendor", {std::byte{0x01}, std::byte{0x02}}});
    return manifest;
}

bool test_roundtrip() {
    const auto encoded = aegra::format::encode_manifest_cbor(make_manifest());
    bool passed = expect(encoded.has_value(), "valid manifest encodes as CBOR");
    if (!encoded) {
        return false;
    }
    const auto decoded = aegra::format::decode_manifest_cbor(encoded.value());
    passed &= expect(decoded.has_value(), "encoded manifest decodes");
    if (!decoded) {
        return false;
    }
    passed &= expect(decoded.value().schema_version == aegra::format::kManifestSchemaVersion,
                     "schema version survives roundtrip");
    passed &= expect(decoded.value().disks.size() == 1, "disk survives roundtrip");
    passed &= expect(decoded.value().volumes.size() == 1, "volume survives roundtrip");
    passed &= expect(decoded.value().volumes.front().volume_id == "volume-guid",
                     "string-key volume metadata survives roundtrip");
    passed &= expect(decoded.value().extensions.front().key == "test.vendor" &&
                         decoded.value().extensions.front().payload.size() == 2,
                     "binary extension survives roundtrip");
    return passed;
}

bool test_rejections() {
    auto invalid_manifest = make_manifest();
    invalid_manifest.volumes.front().extents.front().disk_number = 7;
    const auto invalid = aegra::format::encode_manifest_cbor(invalid_manifest);
    bool passed = expect(!invalid.has_value(), "unknown extent disk is rejected");
    passed &= expect(invalid.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "invalid model has stable error code");

    const std::vector<std::byte> integer_key_map = {std::byte{0xA1}, std::byte{0x01},
                                                    std::byte{0x01}};
    const auto decoded = aegra::format::decode_manifest_cbor(integer_key_map);
    passed &= expect(!decoded.has_value(), "integer CBOR map key is rejected");
    passed &= expect(decoded.error().code == aegra::base::ErrorCode::kCorruptData,
                     "invalid CBOR has corrupt-data error code");
    return passed;
}

int run_tests() { return test_roundtrip() && test_rejections() ? EXIT_SUCCESS : EXIT_FAILURE; }

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
