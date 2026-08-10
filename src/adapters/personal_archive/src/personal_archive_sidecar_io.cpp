#include "personal_archive_sidecar_io.h"
#include "win32_output_file.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/sidecar_crypto.h"
#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/base/error.h"
#include "aegra/format/personal_archive.h"

#include <fstream>
#include <limits>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;

struct ArchiveKeyContext final {
    archive::BackupHeader header;
    archive::MetadataEnvelopeHeader envelope;
};

[[nodiscard]] base::Error error(const base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] char* as_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<std::vector<std::byte>> read_exact(std::ifstream& input,
                                                              const std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "sidecar read exceeds stream limit"));
    }
    std::vector<std::byte> result(size);
    input.read(as_chars(result.data()), static_cast<std::streamsize>(size));
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kIoFailure, "sidecar file is truncated"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] base::Result<void> write_bytes(detail::Win32OutputFile& output,
                                             const std::span<const std::byte> bytes) {
    return output.write(bytes);
}

[[nodiscard]] std::filesystem::path sidecar_path(const std::filesystem::path& archive_path) {
    auto result = archive_path;
    result += ".bhx";
    return result;
}

[[nodiscard]] base::Result<ArchiveKeyContext>
read_archive_key_context(const std::filesystem::path& archive_path) {
    std::ifstream input(archive_path, std::ios::binary);
    if (!input) {
        return base::Result<ArchiveKeyContext>::failure(
            error(base::ErrorCode::kIoFailure, "failed to open archive for sidecar"));
    }
    auto header_bytes = read_exact(input, archive::kBackupHeaderSize);
    if (!header_bytes) {
        return base::Result<ArchiveKeyContext>::failure(header_bytes.error());
    }
    auto header = archive::decode_backup_header(header_bytes.value());
    if (!header) {
        return base::Result<ArchiveKeyContext>::failure(header.error());
    }
    input.seekg(static_cast<std::streamoff>(header.value().cbor_offset), std::ios::beg);
    auto envelope_bytes = read_exact(input, archive::kMetadataEnvelopeHeaderSize);
    if (!envelope_bytes) {
        return base::Result<ArchiveKeyContext>::failure(envelope_bytes.error());
    }
    auto envelope = archive::decode_metadata_envelope_header(envelope_bytes.value());
    if (!envelope) {
        return base::Result<ArchiveKeyContext>::failure(envelope.error());
    }
    return base::Result<ArchiveKeyContext>::success({header.value(), envelope.value()});
}

[[nodiscard]] base::Result<archive::EncodedSidecarHeader>
make_sidecar_aad(archive::SidecarHeader header) {
    header.authentication_tag.fill(std::byte{0});
    return archive::encode_sidecar_header(header);
}

[[nodiscard]] base::Result<void> validate_sidecar_binding(const archive::SidecarHeader& header,
                                                          const ArchiveKeyContext& archive_context,
                                                          const std::uint64_t maximum_size) {
    if (header.file_uuid != archive_context.header.file_uuid ||
        header.block_size != archive_context.header.block_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "sidecar does not match archive"));
    }
    if (header.payload_uncompressed_size > maximum_size ||
        header.payload_stored_size > maximum_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "sidecar exceeds configured limits"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_sidecar_ciphertext(std::ifstream& input, const archive::SidecarHeader& header) {
    auto ciphertext = read_exact(input, static_cast<std::size_t>(header.payload_stored_size));
    if (!ciphertext) {
        return ciphertext;
    }
    if (input.peek() != std::ifstream::traits_type::eof()) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "sidecar has trailing data"));
    }
    return ciphertext;
}

[[nodiscard]] base::Result<archive::SidecarPayload>
decode_sidecar_payload_data(const archive::SidecarHeader& header,
                            const std::span<const std::byte> ciphertext,
                            const ArchiveKeyContext& archive_context,
                            const std::string_view password, const std::uint64_t maximum_size) {
    std::vector<std::byte> compressed;
    if ((header.flags & archive::kSidecarFlagEncrypted) != 0) {
        if (password.empty()) {
            return base::Result<archive::SidecarPayload>::failure(
                error(base::ErrorCode::kUnauthorized, "encrypted sidecar requires a password"));
        }
        auto aad = make_sidecar_aad(header);
        if (!aad) {
            return base::Result<archive::SidecarPayload>::failure(aad.error());
        }
        crypto_sodium::SidecarProtectionContext protection;
        protection.kdf = {archive_context.envelope.kdf_opslimit,
                          archive_context.envelope.kdf_memlimit_bytes,
                          archive_context.envelope.kdf_parameters_version};
        protection.salt = archive_context.envelope.salt;
        protection.nonce = header.nonce;
        auto unlocked = crypto_sodium::unprotect_sidecar_payload(
            ciphertext, header.authentication_tag, password, aad.value(), protection);
        if (!unlocked) {
            return base::Result<archive::SidecarPayload>::failure(unlocked.error());
        }
        compressed = std::move(unlocked).value();
    } else {
        compressed.assign(ciphertext.begin(), ciphertext.end());
    }
    auto plaintext = compression_zstd::decompress(
        compressed, static_cast<std::size_t>(header.payload_uncompressed_size),
        static_cast<std::size_t>(maximum_size));
    if (!plaintext) {
        return base::Result<archive::SidecarPayload>::failure(plaintext.error());
    }
    return archive::decode_sidecar_payload(plaintext.value(), header.volume_count);
}

[[nodiscard]] base::Result<ArchiveSidecar>
decode_sidecar_file(std::ifstream& input, const ArchiveKeyContext& archive_context,
                    const std::string_view password, const std::uint64_t maximum_size) {
    auto header_bytes = read_exact(input, archive::kSidecarHeaderSize);
    if (!header_bytes) {
        return base::Result<ArchiveSidecar>::failure(header_bytes.error());
    }
    auto header = archive::decode_sidecar_header(header_bytes.value());
    if (!header) {
        return base::Result<ArchiveSidecar>::failure(header.error());
    }
    auto binding = validate_sidecar_binding(header.value(), archive_context, maximum_size);
    if (!binding) {
        return base::Result<ArchiveSidecar>::failure(binding.error());
    }
    auto ciphertext = read_sidecar_ciphertext(input, header.value());
    if (!ciphertext) {
        return base::Result<ArchiveSidecar>::failure(ciphertext.error());
    }
    auto payload = decode_sidecar_payload_data(header.value(), ciphertext.value(), archive_context,
                                               password, maximum_size);
    if (!payload) {
        return base::Result<ArchiveSidecar>::failure(payload.error());
    }
    return base::Result<ArchiveSidecar>::success(
        {header.value().block_size, header.value().file_uuid, std::move(payload).value()});
}

} // namespace

namespace detail {

base::Result<void> write_sidecar(const SidecarWriteRequest& request) {
    auto encoded_payload = archive::encode_sidecar_payload(request.payload);
    if (!encoded_payload) {
        return base::Result<void>::failure(encoded_payload.error());
    }
    auto compressed = compression_zstd::compress(encoded_payload.value());
    if (!compressed) {
        return base::Result<void>::failure(compressed.error());
    }
    archive::SidecarHeader header;
    header.block_size = request.block_size;
    header.file_uuid = request.file_uuid;
    if (request.payload.volumes.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInsufficientSpace, "sidecar has too many volumes"));
    }
    header.volume_count = static_cast<std::uint32_t>(request.payload.volumes.size());
    header.payload_uncompressed_size = encoded_payload.value().size();
    header.payload_stored_size = compressed.value().size();
    std::vector<std::byte> stored_payload = std::move(compressed).value();
    if (request.encryption_enabled) {
        if (request.password.empty()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kInvalidArgument, "encrypted sidecar requires a password"));
        }
        auto protection =
            crypto_sodium::create_sidecar_protection_context(request.kdf, request.salt);
        if (!protection) {
            return base::Result<void>::failure(protection.error());
        }
        header.flags = archive::kSidecarFlagEncrypted;
        header.encryption_method = archive::SidecarEncryptionMethod::kXChaCha20Poly1305;
        header.nonce = protection.value().nonce;
        auto aad = make_sidecar_aad(header);
        if (!aad) {
            return base::Result<void>::failure(aad.error());
        }
        auto protected_payload = crypto_sodium::protect_sidecar_payload(
            stored_payload, request.password, aad.value(), protection.value());
        if (!protected_payload) {
            return base::Result<void>::failure(protected_payload.error());
        }
        header.authentication_tag = protected_payload.value().tag;
        stored_payload = std::move(protected_payload).value().ciphertext;
    } else {
        header.flags = 0;
        header.encryption_method = archive::SidecarEncryptionMethod::kNone;
        header.nonce.fill(std::byte{0});
        header.authentication_tag.fill(std::byte{0});
    }
    auto encoded_header = archive::encode_sidecar_header(header);
    if (!encoded_header) {
        return base::Result<void>::failure(encoded_header.error());
    }
    detail::Win32OutputFile output;
    auto opened = output.open(request.destination);
    auto header_written = write_bytes(output, encoded_header.value());
    auto payload_written = write_bytes(output, stored_payload);
    auto flushed = output.flush();
    if (!opened || !header_written || !payload_written || !flushed) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to finalize sidecar file"));
    }
    return base::Result<void>::success();
}

} // namespace detail

base::Result<ArchiveSidecar> load_archive_sidecar(const std::filesystem::path& archive_path,
                                                  const std::string_view password,
                                                  const std::uint64_t maximum_uncompressed_size) {
    if (archive_path.empty() || maximum_uncompressed_size == 0 ||
        maximum_uncompressed_size > (std::numeric_limits<std::size_t>::max)()) {
        return base::Result<ArchiveSidecar>::failure(
            error(base::ErrorCode::kInvalidArgument, "sidecar open request is invalid"));
    }
    auto archive_context = read_archive_key_context(archive_path);
    if (!archive_context) {
        return base::Result<ArchiveSidecar>::failure(archive_context.error());
    }
    std::ifstream input(sidecar_path(archive_path), std::ios::binary);
    if (!input) {
        return base::Result<ArchiveSidecar>::failure(
            error(base::ErrorCode::kNotFound, "archive sidecar does not exist"));
    }
    return decode_sidecar_file(input, archive_context.value(), password, maximum_uncompressed_size);
}

} // namespace aegra::adapters::personal_archive
