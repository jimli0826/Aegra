#include "aegra/adapters/memory/memory_block_io.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"
#include "aegra/pipeline/backup_pipeline.h"
#include "aegra/pipeline/restore_pipeline.h"

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

namespace personal = aegra::adapters::personal_archive;

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
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
        std::filesystem::remove(partial_, ignored);
        std::filesystem::remove(sidecar_, ignored);
        std::filesystem::remove(sidecar_partial_, ignored);
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
#include <algorithm>
#include <array>
