#include "aegra/format/personal_archive.h"
#include "aegra/format/personal_archive_sidecar.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>

namespace {

namespace archive = aegra::format::personal_archive;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_backup_header() {
    archive::BackupHeader header;
    header.file_uuid.front() = std::byte{0xAB};
    header.block_size = 4096;
    header.cbor_size = 180;
    header.first_chunk_offset = archive::kBackupHeaderSize + header.cbor_size;
    header.default_chunk_size = 1024 * 1024;
    header.compression_method = archive::CompressionMethod::kZstandard;

    const auto encoded = archive::encode_backup_header(header);
    bool passed = expect(encoded.has_value(), "backup header encodes");
    if (!encoded) {
        return false;
    }
    passed &= expect(encoded.value()[0] == std::byte{'M'} && encoded.value()[7] == std::byte{'P'},
                     "backup header magic is stable");
    passed &=
        expect(encoded.value()[64] == std::byte{0x00} && encoded.value()[65] == std::byte{0x10},
               "backup header integers are little-endian");
    passed &=
        expect(encoded.value()[122] == std::byte{0x00} && encoded.value().back() == std::byte{0x00},
               "backup header reserved bytes are zero");
    const auto decoded = archive::decode_backup_header(encoded.value());
    passed &= expect(decoded.has_value(), "backup header decodes");
    passed &= expect(decoded.has_value() && decoded.value().cbor_size == 180,
                     "backup header offsets survive roundtrip");
    return passed;
}

bool test_metadata_envelope() {
    archive::MetadataEnvelopeHeader header;
    header.plaintext_size = 64;
    header.ciphertext_size = 64;
    header.kdf_opslimit = 2;
    header.kdf_memlimit_bytes = 64ULL * 1024ULL * 1024ULL;
    header.salt.front() = std::byte{0x11};
    header.nonce.front() = std::byte{0x22};

    const auto encoded = archive::encode_metadata_envelope_header(header);
    bool passed = expect(encoded.has_value(), "metadata envelope encodes");
    if (!encoded) {
        return false;
    }
    passed &=
        expect(encoded.value()[16] == std::byte{0x02}, "XChaCha20-Poly1305 algorithm id is stable");
    passed &= expect(encoded.value()[96] == std::byte{0x02}, "KDF operations are persisted");
    passed &=
        expect(encoded.value()[116] == std::byte{0x00} && encoded.value().back() == std::byte{0x00},
               "envelope reserved bytes are zero");
    const auto decoded = archive::decode_metadata_envelope_header(encoded.value());
    passed &= expect(decoded.has_value(), "metadata envelope decodes");
    passed &= expect(decoded.has_value() &&
                         decoded.value().kdf_memlimit_bytes == 64ULL * 1024ULL * 1024ULL,
                     "KDF memory limit survives roundtrip");
    return passed;
}

bool test_chunk_and_footer() {
    archive::BlockEntry entry;
    entry.logical_block_index = 7;
    entry.data_offset_or_reference = 19;
    entry.stored_size = 100;
    entry.logical_size = 128;
    const auto encoded_entry = archive::encode_block_entry(entry);
    bool passed = expect(encoded_entry.has_value(), "block entry encodes");
    const auto decoded_entry = archive::decode_block_entry(encoded_entry.value());
    passed &= expect(decoded_entry.has_value() && decoded_entry.value().stored_size == 100,
                     "block entry survives roundtrip");

    archive::ChunkHeader chunk;
    chunk.chunk_index = 3;
    chunk.block_entry_count = 1;
    chunk.payload_size = 100;
    const auto encoded_chunk = archive::encode_chunk_header(chunk);
    passed &= expect(encoded_chunk.has_value(), "chunk header encodes");
    passed &= expect(archive::decode_chunk_header(encoded_chunk.value()).has_value(),
                     "chunk header decodes");

    archive::BackupFooter footer;
    footer.chunk_count = 4;
    footer.total_block_count = 8;
    footer.total_payload_size = 400;
    footer.file_size = 2048;
    const auto encoded_footer = archive::encode_backup_footer(footer);
    passed &= expect(encoded_footer.has_value(), "backup footer encodes");
    const auto decoded_footer = archive::decode_backup_footer(encoded_footer.value());
    passed &= expect(decoded_footer.has_value() && decoded_footer.value().file_size == 2048,
                     "backup footer survives roundtrip");
    return passed;
}

bool test_sidecar_codec() {
    archive::SidecarPayload payload;
    archive::SidecarVolume volume;
    volume.volume_index = 7;
    archive::SidecarRecord data_record;
    data_record.hash.front() = std::byte{0xA5};
    volume.records.push_back(data_record);
    volume.records.push_back({archive::SidecarBlockState::kZero, {}});
    payload.volumes.push_back(volume);

    const auto encoded_payload = archive::encode_sidecar_payload(payload);
    bool passed = expect(encoded_payload.has_value(), "sidecar payload encodes");
    if (!encoded_payload) {
        return false;
    }
    const auto decoded_payload = archive::decode_sidecar_payload(encoded_payload.value(), 1);
    passed &= expect(decoded_payload.has_value(), "sidecar payload decodes");
    passed &= expect(decoded_payload.has_value() &&
                         decoded_payload.value().volumes.front().records.size() == 2 &&
                         decoded_payload.value().volumes.front().records.front().hash.front() ==
                             std::byte{0xA5},
                     "sidecar records survive roundtrip");
    payload.volumes.front().records.back().hash.front() = std::byte{0x01};
    passed &= expect(!archive::encode_sidecar_payload(payload).has_value(),
                     "non-data sidecar records reject non-zero hashes");

    archive::SidecarHeader header;
    header.block_size = 4096;
    header.file_uuid.front() = std::byte{0x11};
    header.volume_count = 1;
    header.payload_uncompressed_size = encoded_payload.value().size();
    header.payload_stored_size = 42;
    header.nonce.front() = std::byte{0x22};
    header.authentication_tag.back() = std::byte{0x33};
    const auto encoded_header = archive::encode_sidecar_header(header);
    passed &= expect(encoded_header.has_value() && encoded_header.value().size() == 96,
                     "sidecar header has stable 96-byte wire size");
    const auto decoded_header = archive::decode_sidecar_header(encoded_header.value());
    passed &= expect(decoded_header.has_value() &&
                         decoded_header.value().authentication_tag.back() == std::byte{0x33},
                     "sidecar header survives roundtrip");
    auto unsupported_header = encoded_header.value();
    unsupported_header[32] = std::byte{0x7F};
    passed &= expect(!archive::decode_sidecar_header(unsupported_header).has_value(),
                     "unsupported sidecar hash algorithm is rejected");
    return passed;
}

int run_tests() {
    return test_backup_header() && test_metadata_envelope() && test_chunk_and_footer() &&
                   test_sidecar_codec()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
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
