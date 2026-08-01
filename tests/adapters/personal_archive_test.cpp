#include "aegra/adapters/memory/memory_block_io.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"
#include "aegra/pipeline/backup_pipeline.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <string>
#include <vector>

namespace {

namespace personal = aegra::adapters::personal_archive;

std::filesystem::path split_part_path(const std::filesystem::path& primary,
                                      const std::uint32_t part_index) {
    if (part_index == 0) {
        return primary;
    }
    std::ostringstream suffix;
    suffix << '.' << std::setw(3) << std::setfill('0') << part_index;
    auto result = primary;
    result += suffix.str();
    return result;
}

class TemporaryArchive final {
  public:
    TemporaryArchive() {
        const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-personal-archive-" + std::to_string(unique) + ".bkf");
        partial_ = path_;
        partial_ += ".partial";
        sidecar_ = path_;
        sidecar_ += ".bhx";
        sidecar_partial_ = sidecar_;
        sidecar_partial_ += ".partial";
    }

    ~TemporaryArchive() noexcept {
        try {
            std::error_code ignored;
            std::filesystem::remove(path_, ignored);
            std::filesystem::remove(partial_, ignored);
            std::filesystem::remove(sidecar_, ignored);
            std::filesystem::remove(sidecar_partial_, ignored);
            for (std::uint32_t index = 1; index < 32; ++index) {
                const auto part = split_part_path(path_, index);
                std::filesystem::remove(part, ignored);
                auto part_partial = part;
                part_partial += ".partial";
                std::filesystem::remove(part_partial, ignored);
                auto missing = part;
                missing += ".missing";
                std::filesystem::remove(missing, ignored);
            }
        } catch (...) {
            std::fputs("[WARN] temporary archive cleanup was incomplete\n", stderr);
        }
    }
    TemporaryArchive(const TemporaryArchive&) = delete;
    TemporaryArchive& operator=(const TemporaryArchive&) = delete;
    TemporaryArchive(TemporaryArchive&&) = delete;
    TemporaryArchive& operator=(TemporaryArchive&&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
    std::filesystem::path partial_;
    std::filesystem::path sidecar_;
    std::filesystem::path sidecar_partial_;
};

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::vector<std::byte> make_source_data() {
    std::vector<std::byte> result(6ULL * 4096ULL + 123ULL);
    for (std::size_t index = 0; index < result.size(); ++index) {
        result[index] = static_cast<std::byte>(1 + (index / 97) % 31);
    }
    std::fill(result.begin() + static_cast<std::ptrdiff_t>(4096),
              result.begin() + static_cast<std::ptrdiff_t>(4ULL * 4096ULL), std::byte{0});
    return result;
}

template <std::size_t Size>
std::array<std::byte, Size> read_at(std::ifstream& input, const std::uint64_t offset) {
    std::array<std::byte, Size> bytes{};
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) test stream boundary.
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return bytes;
}

template <std::size_t Size>
bool write_at(const std::filesystem::path& path, const std::uint64_t offset,
              const std::array<std::byte, Size>& bytes) {
    std::fstream output(path, std::ios::binary | std::ios::in | std::ios::out);
    output.seekp(static_cast<std::streamoff>(offset), std::ios::beg);
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) test stream boundary.
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    output.flush();
    return output.good();
}

bool archive_contains_zero_run(const std::filesystem::path& path) {
    namespace archive = aegra::format::personal_archive;
    std::ifstream input(path, std::ios::binary);
    const auto header =
        archive::decode_backup_header(read_at<archive::kBackupHeaderSize>(input, 0));
    if (!header) {
        return false;
    }
    auto offset = header.value().first_chunk_offset;
    for (std::uint64_t chunk_index = 0; chunk_index < 2; ++chunk_index) {
        const auto chunk =
            archive::decode_chunk_header(read_at<archive::kChunkHeaderSize>(input, offset));
        if (!chunk) {
            return false;
        }
        const auto entries_offset = offset + archive::kChunkHeaderSize;
        for (std::uint32_t index = 0; index < chunk.value().block_entry_count; ++index) {
            const auto entry = archive::decode_block_entry(read_at<archive::kBlockEntrySize>(
                input, entries_offset + index * archive::kBlockEntrySize));
            if (entry && entry.value().flags == archive::kBlockFlagZero &&
                entry.value().logical_size >= 2 && entry.value().stored_size == 0) {
                return true;
            }
        }
        offset = entries_offset + chunk.value().block_entry_count * archive::kBlockEntrySize +
                 chunk.value().payload_size;
    }
    return false;
}

aegra::format::Manifest make_manifest(const std::uint64_t logical_size) {
    aegra::format::Manifest manifest;
    manifest.backup_job.created_utc = "2026-08-01T09:00:00Z";
    manifest.backup_job.application_version = "0.1.0";
    manifest.system.hostname = "pipeline-test";
    aegra::format::Volume volume;
    volume.volume_id = "test-volume";
    volume.volume_guid = "test-volume";
    volume.total_size = logical_size;
    volume.cluster_size = 4096;
    manifest.volumes.push_back(std::move(volume));
    return manifest;
}

bool run_roundtrip(const TemporaryArchive& archive) {
    const auto source_data = make_source_data();
    const auto manifest = make_manifest(source_data.size());
    personal::ArchiveCreateRequest create_request{archive.path(), manifest, "test-password"};
    create_request.file_uuid.front() = std::byte{0x11};
    create_request.backup_set_uuid.front() = std::byte{0x22};
    create_request.block_size = 4096;
    create_request.chunk_size = 8192;
    create_request.kdf_parameters = {2, 64ULL * 1024ULL * 1024ULL};
    auto session = personal::PersonalArchiveSession::create(create_request);
    bool passed = expect(session.has_value(), "personal archive session is created");
    if (!session) {
        return false;
    }

    aegra::adapters::memory::MemoryBlockSource source(source_data);
    aegra::pipeline::BackupPipeline backup(source, *session.value());
    const auto backup_result = backup.run({"archive-backup", 8192, 16384}, {});
    passed &= expect(backup_result.has_value(), "pipeline writes and commits personal archive");
    passed &= expect(std::filesystem::exists(archive.path()), "committed archive is published");
    auto sidecar_path = archive.path();
    sidecar_path += ".bhx";
    passed &= expect(std::filesystem::exists(sidecar_path), "encrypted sidecar is published");
    passed &= expect(archive_contains_zero_run(archive.path()),
                     "consecutive zero blocks use a ZERO run entry");
    if (!backup_result) {
        return false;
    }

    auto reader = personal::PersonalArchiveReader::open({archive.path(), "test-password"});
    passed &= expect(reader.has_value(), "personal archive opens with correct password");
    if (!reader) {
        return false;
    }
    passed &= expect(reader.value()->manifest().volumes.front().volume_id == "test-volume",
                     "encrypted string-key manifest is restored");
    const auto sidecar = personal::load_archive_sidecar(archive.path(), "test-password");
    passed &= expect(sidecar.has_value(), "archive sidecar authenticates and decodes");
    if (sidecar) {
        const auto& records = sidecar.value().payload.volumes.front().records;
        passed &= expect(records.size() == 7, "sidecar contains one record per logical block");
        passed &= expect(
            records[1].state == aegra::format::personal_archive::SidecarBlockState::kZero &&
                records[2].state == aegra::format::personal_archive::SidecarBlockState::kZero &&
                records[3].state == aegra::format::personal_archive::SidecarBlockState::kZero,
            "sidecar preserves zero block state");
    }
    aegra::adapters::memory::MemoryBlockSink sink(source_data.size());
    aegra::pipeline::RestorePipeline restore(*reader.value(), sink);
    const auto restore_result = restore.run({"archive-restore", 16384}, {});
    passed &= expect(restore_result.has_value(), "pipeline restores personal archive");
    passed &= expect(sink.snapshot() == source_data, "restored bytes equal source bytes");
    return passed;
}

bool restore_matches(const std::filesystem::path& path, const std::vector<std::byte>& source_data) {
    auto reader = personal::PersonalArchiveReader::open({path, "test-password"});
    if (!reader) {
        return false;
    }
    aegra::adapters::memory::MemoryBlockSink sink(source_data.size());
    aegra::pipeline::RestorePipeline restore(*reader.value(), sink);
    const auto restored = restore.run({"split-restore", 16384}, {});
    return restored.has_value() && sink.snapshot() == source_data;
}

bool continuation_header_is_valid(const std::filesystem::path& path) {
    namespace archive = aegra::format::personal_archive;
    std::ifstream input(split_part_path(path, 1), std::ios::binary);
    const auto header =
        archive::decode_backup_header(read_at<archive::kBackupHeaderSize>(input, 0));
    return header.has_value() && header.value().split_part_index == 1 &&
           header.value().cbor_size == 0 &&
           header.value().first_chunk_offset == archive::kBackupHeaderSize;
}

bool missing_part_is_rejected(const std::filesystem::path& path) {
    const auto part = split_part_path(path, 1);
    auto missing = part;
    missing += ".missing";
    std::error_code filesystem_error;
    std::filesystem::rename(part, missing, filesystem_error);
    if (filesystem_error) {
        return false;
    }
    const auto opened = personal::PersonalArchiveReader::open({path, "test-password"});
    std::filesystem::rename(missing, part, filesystem_error);
    return !opened.has_value() && !filesystem_error;
}

bool mismatched_part_identity_is_rejected(const std::filesystem::path& path) {
    namespace archive = aegra::format::personal_archive;
    const auto part = split_part_path(path, 1);
    std::ifstream input(part, std::ios::binary);
    const auto original = read_at<archive::kBackupHeaderSize>(input, 0);
    auto header = archive::decode_backup_header(original);
    input.close();
    if (!header) {
        return false;
    }
    header.value().file_uuid.front() ^= std::byte{0x01};
    auto changed = archive::encode_backup_header(header.value());
    if (!changed || !write_at(part, 0, changed.value())) {
        return false;
    }
    const auto opened = personal::PersonalArchiveReader::open({path, "test-password"});
    const bool restored = write_at(part, 0, original);
    return !opened.has_value() && restored;
}

bool invalid_final_footer_is_rejected(const std::filesystem::path& path) {
    namespace archive = aegra::format::personal_archive;
    auto final_part = path;
    for (std::uint32_t index = 1; index < 32; ++index) {
        const auto candidate = split_part_path(path, index);
        if (!std::filesystem::exists(candidate)) {
            break;
        }
        final_part = candidate;
    }
    std::ifstream input(final_part, std::ios::binary);
    input.seekg(0, std::ios::end);
    const auto position = input.tellg();
    if (position < 0) {
        return false;
    }
    const auto size = static_cast<std::uint64_t>(static_cast<std::streamoff>(position));
    if (size < archive::kBackupFooterSize) {
        return false;
    }
    const auto offset = size - archive::kBackupFooterSize;
    const auto original = read_at<archive::kBackupFooterSize>(input, offset);
    input.close();
    auto damaged = original;
    damaged.front() ^= std::byte{0x01};
    if (!write_at(final_part, offset, damaged)) {
        return false;
    }
    const auto opened = personal::PersonalArchiveReader::open({path, "test-password"});
    const bool restored = write_at(final_part, offset, original);
    return !opened.has_value() && restored;
}

bool run_split_roundtrip(const TemporaryArchive& archive) {
    const auto source_data = make_source_data();
    const auto manifest = make_manifest(source_data.size());
    personal::ArchiveCreateRequest create_request{archive.path(), manifest, "test-password"};
    create_request.file_uuid.front() = std::byte{0x31};
    create_request.backup_set_uuid.front() = std::byte{0x32};
    create_request.block_size = 4096;
    create_request.chunk_size = 8192;
    create_request.split_size_bytes = 800;
    create_request.kdf_parameters = {2, 64ULL * 1024ULL * 1024ULL};
    auto session = personal::PersonalArchiveSession::create(create_request);
    bool passed = expect(session.has_value(), "split archive session is created");
    if (!session) {
        return false;
    }
    aegra::adapters::memory::MemoryBlockSource source(source_data);
    aegra::pipeline::BackupPipeline backup(source, *session.value());
    const auto result = backup.run({"split-backup", 8192, 16384}, {});
    passed &= expect(result.has_value(), "split archive commits through the pipeline");
    passed &= expect(std::filesystem::exists(split_part_path(archive.path(), 1)),
                     "split archive publishes continuation parts");
    passed &= expect(continuation_header_is_valid(archive.path()),
                     "continuation part has a metadata-free indexed header");
    passed &= expect(restore_matches(archive.path(), source_data),
                     "reader restores chunks across split part boundaries");
    personal::ArchiveOpenRequest limited{archive.path(), "test-password"};
    limited.maximum_split_parts = 1;
    passed &= expect(!personal::PersonalArchiveReader::open(limited).has_value(),
                     "reader enforces the split part discovery limit");
    passed &= expect(missing_part_is_rejected(archive.path()),
                     "reader rejects a missing continuation part");
    passed &= expect(mismatched_part_identity_is_rejected(archive.path()),
                     "reader rejects a continuation part from another archive");
    passed &= expect(invalid_final_footer_is_rejected(archive.path()),
                     "reader rejects a split archive without a valid final footer");
    passed &= expect(personal::load_archive_sidecar(archive.path(), "test-password").has_value(),
                     "split archive sidecar authenticates against the primary header");
    return passed;
}

bool run_split_abort_cleanup(const TemporaryArchive& archive) {
    const auto source_data = make_source_data();
    const auto manifest = make_manifest(source_data.size());
    personal::ArchiveCreateRequest request{archive.path(), manifest, "test-password"};
    request.block_size = 4096;
    request.chunk_size = 8192;
    request.split_size_bytes = 800;
    request.kdf_parameters = {2, 64ULL * 1024ULL * 1024ULL};
    auto session = personal::PersonalArchiveSession::create(request);
    if (!session) {
        return false;
    }
    const auto data = std::span<const std::byte>(source_data);
    auto first = session.value()->write_chunk({{0, 0, 8192, 8192}, data.first(8192)}, {});
    auto second =
        session.value()->write_chunk({{1, 8192, 8192, 8192}, data.subspan(8192, 8192)}, {});
    session.value()->abort();
    auto primary_partial = archive.path();
    primary_partial += ".partial";
    auto continuation_partial = split_part_path(archive.path(), 1);
    continuation_partial += ".partial";
    return first.has_value() && second.has_value() && !std::filesystem::exists(archive.path()) &&
           !std::filesystem::exists(primary_partial) &&
           !std::filesystem::exists(split_part_path(archive.path(), 1)) &&
           !std::filesystem::exists(continuation_partial);
}

bool write_test_archive(const TemporaryArchive& archive, const std::vector<std::byte>& source_data,
                        const aegra::format::Manifest& manifest, const std::byte file_id,
                        const std::filesystem::path& parent = {}) {
    personal::ArchiveCreateRequest request{archive.path(), manifest, "test-password"};
    request.file_uuid.front() = file_id;
    if (manifest.backup_job.backup_type == aegra::format::BackupType::kFull) {
        request.backup_set_uuid.front() = std::byte{0x51};
    }
    request.block_size = 4096;
    request.chunk_size = 8192;
    request.kdf_parameters = {2, 64ULL * 1024ULL * 1024ULL};
    if (!parent.empty()) {
        request.parent_source = parent;
        request.parent_password = "test-password";
    }
    auto session = personal::PersonalArchiveSession::create(request);
    if (!session) {
        return false;
    }
    aegra::adapters::memory::MemoryBlockSource source(source_data);
    aegra::pipeline::BackupPipeline backup(source, *session.value());
    return backup.run({"incremental-backup", 8192, 16384}, {}).has_value();
}

bool wrong_parent_password_is_rejected(const TemporaryArchive& archive,
                                       const aegra::format::Manifest& manifest,
                                       const std::filesystem::path& parent) {
    personal::ArchiveCreateRequest request{archive.path(), manifest, "test-password"};
    request.file_uuid.front() = std::byte{0x42};
    request.block_size = 4096;
    request.chunk_size = 8192;
    request.kdf_parameters = {2, 64ULL * 1024ULL * 1024ULL};
    request.parent_source = parent;
    request.parent_password = "wrong-password";
    return !personal::PersonalArchiveSession::create(request).has_value();
}

void remove_sidecar(const std::filesystem::path& archive) {
    auto sidecar = archive;
    sidecar += ".bhx";
    std::error_code ignored;
    std::filesystem::remove(sidecar, ignored);
}

std::vector<std::byte> make_incremental_data(const std::vector<std::byte>& base) {
    auto result = base;
    result.front() ^= std::byte{0x5A};
    std::fill(result.begin() + static_cast<std::ptrdiff_t>(4ULL * 4096ULL),
              result.begin() + static_cast<std::ptrdiff_t>(5ULL * 4096ULL), std::byte{0});
    return result;
}

bool incremental_layer_is_sparse(const TemporaryArchive& base, const TemporaryArchive& incremental,
                                 const std::vector<std::byte>& latest) {
    auto reader = personal::PersonalArchiveReader::open({incremental.path(), "test-password"});
    if (!reader || reader.value()->chunk_count() != 2) {
        return false;
    }
    const auto& identity = reader.value()->identity();
    auto first = reader.value()->describe_chunk(0);
    auto second = reader.value()->describe_chunk(1);
    const bool identity_matches = identity.backup_type == aegra::format::BackupType::kIncremental &&
                                  identity.parent_uuid.front() == std::byte{0x41} &&
                                  identity.backup_set_uuid.front() == std::byte{0x51};
    const bool sparse = first && second && first.value().logical_offset == 0 &&
                        second.value().logical_offset == 4ULL * 4096ULL;
    auto sidecar = personal::load_archive_sidecar(incremental.path(), "test-password");
    const bool complete_sidecar = sidecar &&
                                  sidecar.value().payload.volumes.front().records.size() == 7 &&
                                  sidecar.value().payload.volumes.front().records[4].state ==
                                      aegra::format::personal_archive::SidecarBlockState::kZero;
    aegra::adapters::memory::MemoryBlockSink sink(latest.size());
    aegra::pipeline::RestorePipeline direct_restore(*reader.value(), sink);
    const bool direct_rejected =
        !direct_restore.run({"invalid-direct-incremental", 16384}, {}).has_value();
    return identity_matches && sparse && complete_sidecar && direct_rejected &&
           std::filesystem::exists(base.path());
}

bool chain_restores(const std::vector<const TemporaryArchive*>& archives,
                    const std::vector<std::byte>& expected) {
    personal::ArchiveChainOpenRequest request;
    for (const auto* archive : archives) {
        request.layers.push_back({archive->path(), "test-password"});
    }
    auto reader = personal::PersonalArchiveChainReader::open(request);
    if (!reader) {
        return false;
    }
    aegra::adapters::memory::MemoryBlockSink sink(expected.size());
    aegra::pipeline::RestorePipeline restore(*reader.value(), sink);
    return restore.run({"chain-restore", 16384}, {}).has_value() && sink.snapshot() == expected;
}

bool run_incremental_roundtrip(const TemporaryArchive& base, const TemporaryArchive& incremental,
                               const TemporaryArchive& unchanged) {
    const auto base_data = make_source_data();
    const auto latest = make_incremental_data(base_data);
    auto full_manifest = make_manifest(base_data.size());
    if (!write_test_archive(base, base_data, full_manifest, std::byte{0x41})) {
        return false;
    }
    auto incremental_manifest = make_manifest(latest.size());
    incremental_manifest.backup_job.backup_type = aegra::format::BackupType::kIncremental;
    if (!wrong_parent_password_is_rejected(incremental, incremental_manifest, base.path())) {
        return false;
    }
    if (!write_test_archive(incremental, latest, incremental_manifest, std::byte{0x42},
                            base.path())) {
        return false;
    }
    bool passed = incremental_layer_is_sparse(base, incremental, latest);
    passed &= chain_restores({&base, &incremental}, latest);
    auto unchanged_manifest = make_manifest(latest.size());
    unchanged_manifest.backup_job.backup_type = aegra::format::BackupType::kIncremental;
    passed &= write_test_archive(unchanged, latest, unchanged_manifest, std::byte{0x43},
                                 incremental.path());
    auto unchanged_reader =
        personal::PersonalArchiveReader::open({unchanged.path(), "test-password"});
    passed &= unchanged_reader && unchanged_reader.value()->chunk_count() == 0;
    auto unchanged_sidecar = personal::load_archive_sidecar(unchanged.path(), "test-password");
    passed &=
        unchanged_sidecar && unchanged_sidecar.value().payload.volumes.front().records.size() == 7;
    remove_sidecar(base.path());
    remove_sidecar(incremental.path());
    remove_sidecar(unchanged.path());
    passed &= chain_restores({&base, &incremental, &unchanged}, latest);
    personal::ArchiveChainOpenRequest missing_middle;
    missing_middle.layers = {{base.path(), "test-password"}, {unchanged.path(), "test-password"}};
    passed &= !personal::PersonalArchiveChainReader::open(missing_middle).has_value();
    return passed;
}

int run_tests() {
    TemporaryArchive archive;
    bool passed = run_roundtrip(archive);
    const auto wrong_password =
        personal::PersonalArchiveReader::open({archive.path(), "wrong-password"});
    passed &= expect(!wrong_password.has_value(), "wrong archive password is rejected");
    const auto wrong_sidecar_password =
        personal::load_archive_sidecar(archive.path(), "wrong-password");
    passed &= expect(!wrong_sidecar_password.has_value(), "wrong sidecar password is rejected");
    personal::ArchiveOpenRequest limited_request{archive.path(), "test-password"};
    limited_request.maximum_chunk_logical_size = 4096;
    passed &= expect(!personal::PersonalArchiveReader::open(limited_request).has_value(),
                     "expanded ZERO runs obey the logical chunk limit");
    auto sidecar_path = archive.path();
    sidecar_path += ".bhx";
    std::error_code ignored;
    std::filesystem::remove(sidecar_path, ignored);
    passed &=
        expect(personal::PersonalArchiveReader::open({archive.path(), "test-password"}).has_value(),
               "archive restore remains available without the optional sidecar");
    TemporaryArchive split_archive;
    passed &= run_split_roundtrip(split_archive);
    TemporaryArchive aborted_split_archive;
    passed &= expect(run_split_abort_cleanup(aborted_split_archive),
                     "aborting a split session removes every partial part");
    TemporaryArchive base_archive;
    TemporaryArchive incremental_archive;
    TemporaryArchive unchanged_archive;
    passed &=
        expect(run_incremental_roundtrip(base_archive, incremental_archive, unchanged_archive),
               "incremental layers and chain reader restore the latest complete state");
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
