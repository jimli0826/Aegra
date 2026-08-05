#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_chunk_builder.h"
#include "personal_archive_payload.h"
#include "personal_archive_sidecar_io.h"

#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/adapters/crypto_sodium/secure_string.h"
#include "aegra/base/error.h"
#include "aegra/format/manifest_codec.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <system_error>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;

inline constexpr std::uint64_t kMaximumMetadataSize = 64ULL * 1024ULL * 1024ULL;

struct ArchivePreamble final {
    archive::BackupHeader logical_header;
    archive::EncodedBackupHeader header;
    archive::EncodedMetadataEnvelopeHeader envelope;
    crypto_sodium::ProtectedMetadata metadata;
};

struct PartArtifact final {
    std::filesystem::path destination;
    std::filesystem::path partial;
};

struct IncrementalBaseline final {
    ArchiveIdentity identity;
    std::vector<archive::SidecarRecord> records;
};

struct SourceWriteState final {
    std::uint32_t source_index{0};
    std::uint64_t logical_size{0};
    std::uint64_t next_logical_offset{0};
    std::uint64_t next_input_chunk_index{0};
    std::vector<archive::SidecarRecord> sidecar_records;
    std::vector<archive::SidecarRecord> baseline_records;
};

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] const char* as_chars(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<const char*>(value);
}

[[nodiscard]] base::Result<void> validate_create_geometry(const ArchiveCreateRequest& request) {
    if (request.destination.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive path is required"));
    }
    if (request.encryption_enabled && request.password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "encrypted archive requires a password"));
    }
    if (!request.encryption_enabled && !request.password.empty()) {
        return base::Result<void>::failure(error(
            base::ErrorCode::kInvalidArgument, "unencrypted archive must not supply a password"));
    }
    if (request.block_size == 0 || request.chunk_size < request.block_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive block and chunk sizes are invalid"));
    }
    if (request.split_size_bytes != 0 &&
        request.split_size_bytes < archive::kBackupHeaderSize + archive::kBackupFooterSize) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive split size is too small"));
    }
    if (request.manifest.volumes.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive requires at least one source volume"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_backup_relationship(const ArchiveCreateRequest& request) {
    const auto backup_type = request.manifest.backup_job.backup_type;
    if (backup_type == format::BackupType::kDifferential) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "differential archives are not implemented"));
    }
    if (backup_type != format::BackupType::kFull &&
        backup_type != format::BackupType::kIncremental) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive backup type is invalid"));
    }
    if (backup_type == format::BackupType::kIncremental && request.manifest.volumes.size() != 1) {
        return base::Result<void>::failure(error(
            base::ErrorCode::kInvalidArgument,
            "multi-volume incremental archives are not implemented"));
    }
    if (backup_type == format::BackupType::kFull) {
        if (request.parent_source.empty() && request.parent_password.empty()) {
            return base::Result<void>::success();
        }
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "full archive must not specify a parent"));
    }
    if (request.parent_source.empty() ||
        (request.encryption_enabled && request.parent_password.empty())) {
        return base::Result<void>::failure(error(base::ErrorCode::kInvalidArgument,
                                                 "incremental archive requires a complete parent"));
    }
    std::error_code destination_error;
    std::error_code parent_error;
    auto destination = std::filesystem::absolute(request.destination, destination_error)
                           .lexically_normal()
                           .native();
    auto parent =
        std::filesystem::absolute(request.parent_source, parent_error).lexically_normal().native();
#ifdef _WIN32
    std::ranges::transform(destination, destination.begin(),
                           [](wchar_t value) { return std::towlower(value); });
    std::ranges::transform(parent, parent.begin(),
                           [](wchar_t value) { return std::towlower(value); });
#endif
    if (destination_error || parent_error || destination == parent) {
        return base::Result<void>::failure(error(
            base::ErrorCode::kConflict, "incremental destination must differ from its parent"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_create_request(const ArchiveCreateRequest& request) {
    auto geometry = validate_create_geometry(request);
    if (!geometry) {
        return geometry;
    }
    auto relationship = validate_backup_relationship(request);
    if (!relationship) {
        return relationship;
    }
    return format::validate_manifest(request.manifest);
}

[[nodiscard]] std::uint64_t block_count(const std::uint64_t logical_size,
                                        const std::uint32_t block_size) noexcept {
    return 1 + (logical_size - 1) / block_size;
}

[[nodiscard]] bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] base::Result<std::optional<IncrementalBaseline>>
load_incremental_baseline(const ArchiveCreateRequest& request, const format::Volume& volume) {
    if (request.manifest.backup_job.backup_type == format::BackupType::kFull) {
        return base::Result<std::optional<IncrementalBaseline>>::success(std::nullopt);
    }
    auto parent = PersonalArchiveReader::open({request.parent_source, request.parent_password});
    if (!parent) {
        return base::Result<std::optional<IncrementalBaseline>>::failure(parent.error());
    }
    auto sidecar = load_archive_sidecar(request.parent_source, request.parent_password);
    if (!sidecar) {
        return base::Result<std::optional<IncrementalBaseline>>::failure(sidecar.error());
    }
    const auto& parent_manifest = parent.value()->manifest();
    const auto& identity = parent.value()->identity();
    const bool volume_matches = parent_manifest.volumes.size() == 1 &&
                                parent_manifest.volumes.front().volume_id == volume.volume_id &&
                                parent_manifest.volumes.front().total_size == volume.total_size;
    const bool sidecar_matches =
        sidecar.value().block_size == request.block_size &&
        sidecar.value().payload.volumes.size() == 1 &&
        sidecar.value().payload.volumes.front().volume_index == volume.volume_index &&
        sidecar.value().payload.volumes.front().records.size() ==
            block_count(volume.total_size, request.block_size);
    const bool requested_set_matches = is_zero_uuid(request.backup_set_uuid) ||
                                       request.backup_set_uuid == identity.backup_set_uuid;
    if (!volume_matches || !sidecar_matches || !requested_set_matches ||
        identity.block_size != request.block_size || identity.file_uuid == request.file_uuid) {
        return base::Result<std::optional<IncrementalBaseline>>::failure(
            error(base::ErrorCode::kConflict, "incremental parent does not match the source"));
    }
    IncrementalBaseline baseline{identity,
                                 std::move(sidecar).value().payload.volumes.front().records};
    return base::Result<std::optional<IncrementalBaseline>>::success(std::move(baseline));
}

[[nodiscard]] std::vector<std::byte>
make_authenticated_data(const archive::EncodedBackupHeader& header,
                        const archive::EncodedMetadataEnvelopeHeader& envelope) {
    std::vector<std::byte> result;
    result.reserve(header.size() + envelope.size());
    result.insert(result.end(), header.begin(), header.end());
    result.insert(result.end(), envelope.begin(), envelope.end());
    return result;
}

[[nodiscard]] archive::MetadataEnvelopeHeader
make_envelope(const crypto_sodium::MetadataProtectionContext& context,
              const std::uint64_t plaintext_size) {
    archive::MetadataEnvelopeHeader envelope;
    envelope.plaintext_size = plaintext_size;
    envelope.ciphertext_size = plaintext_size;
    envelope.salt = context.salt;
    envelope.nonce = context.nonce;
    envelope.kdf_opslimit = context.kdf.opslimit;
    envelope.kdf_memlimit_bytes = context.kdf.memlimit_bytes;
    envelope.kdf_parameters_version = context.kdf.parameters_version;
    return envelope;
}

[[nodiscard]] archive::BackupHeader
make_header(const ArchiveCreateRequest& request, const std::uint64_t metadata_wire_size,
            const std::optional<IncrementalBaseline>& baseline) {
    archive::BackupHeader header;
    header.file_uuid = request.file_uuid;
    header.backup_set_uuid =
        baseline ? baseline->identity.backup_set_uuid : request.backup_set_uuid;
    header.parent_uuid = baseline ? baseline->identity.file_uuid : std::array<std::byte, 16>{};
    header.block_size = request.block_size;
    header.flags = baseline ? archive::kBackupFlagIncremental : archive::kBackupFlagFull;
    if (request.encryption_enabled) {
        header.flags |= archive::kBackupFlagEncrypted;
        header.encryption_method = archive::PayloadEncryptionMethod::kXChaCha20Poly1305;
    } else {
        header.encryption_method = archive::PayloadEncryptionMethod::kNone;
    }
    if (request.split_size_bytes != 0) {
        header.flags |= archive::kBackupFlagSplit;
        header.split_size_bytes = request.split_size_bytes;
    }
    header.cbor_size = metadata_wire_size;
    header.first_chunk_offset = header.cbor_offset + header.cbor_size;
    header.default_chunk_size = request.chunk_size;
    header.compression_method = archive::CompressionMethod::kZstandard;
    return header;
}

[[nodiscard]] base::Result<ArchivePreamble>
prepare_preamble(const ArchiveCreateRequest& request,
                 const std::optional<IncrementalBaseline>& baseline) {
    auto cbor = format::encode_manifest_cbor(request.manifest);
    if (!cbor) {
        return base::Result<ArchivePreamble>::failure(cbor.error());
    }
    if (cbor.value().size() > kMaximumMetadataSize) {
        return base::Result<ArchivePreamble>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive metadata exceeds product limit"));
    }
    if (!request.encryption_enabled) {
        archive::MetadataEnvelopeHeader logical_envelope;
        logical_envelope.flags = 0;
        logical_envelope.encryption_method = archive::MetadataEncryptionMethod::kNone;
        logical_envelope.kdf_method = archive::MetadataKdfMethod::kNone;
        logical_envelope.nonce_size = 0;
        logical_envelope.tag_size = 0;
        logical_envelope.plaintext_size = cbor.value().size();
        logical_envelope.ciphertext_size = cbor.value().size();
        logical_envelope.kdf_parameters_version = 0;
        const auto wire_size =
            archive::kMetadataEnvelopeHeaderSize + cbor.value().size(); // no tag
        auto logical_header = make_header(request, wire_size, baseline);
        auto header = archive::encode_backup_header(logical_header);
        auto envelope = archive::encode_metadata_envelope_header(logical_envelope);
        if (!header || !envelope) {
            return base::Result<ArchivePreamble>::failure(!header ? header.error()
                                                                  : envelope.error());
        }
        crypto_sodium::ProtectedMetadata metadata;
        metadata.ciphertext = std::move(cbor).value();
        return base::Result<ArchivePreamble>::success(
            {logical_header, header.value(), envelope.value(), std::move(metadata)});
    }
    const crypto_sodium::KdfParameters kdf{request.kdf_parameters.opslimit,
                                           request.kdf_parameters.memlimit_bytes,
                                           crypto_sodium::kKdfParametersVersion};
    auto context = crypto_sodium::create_metadata_protection_context(kdf);
    if (!context) {
        return base::Result<ArchivePreamble>::failure(context.error());
    }
    const auto wire_size = archive::kMetadataEnvelopeHeaderSize + cbor.value().size() +
                           crypto_sodium::kMetadataTagSize;
    auto logical_header = make_header(request, wire_size, baseline);
    auto logical_envelope = make_envelope(context.value(), cbor.value().size());
    auto header = archive::encode_backup_header(logical_header);
    auto envelope = archive::encode_metadata_envelope_header(logical_envelope);
    if (!header || !envelope) {
        return base::Result<ArchivePreamble>::failure(!header ? header.error() : envelope.error());
    }
    const auto aad = make_authenticated_data(header.value(), envelope.value());
    auto metadata =
        crypto_sodium::protect_metadata(cbor.value(), request.password, aad, context.value());
    if (!metadata) {
        return base::Result<ArchivePreamble>::failure(metadata.error());
    }
    return base::Result<ArchivePreamble>::success(
        {logical_header, header.value(), envelope.value(), std::move(metadata).value()});
}

[[nodiscard]] base::Result<std::unique_ptr<crypto_sodium::PayloadCipher>>
create_payload_cipher(const ArchiveCreateRequest& request, const ArchivePreamble& preamble) {
    if (!request.encryption_enabled) {
        return base::Result<std::unique_ptr<crypto_sodium::PayloadCipher>>::success({});
    }
    return crypto_sodium::PayloadCipher::create(request.password, preamble.metadata.kdf,
                                                preamble.metadata.salt);
}

[[nodiscard]] base::Result<void> write_bytes(std::ofstream& output,
                                             std::span<const std::byte> bytes) {
    output.write(as_chars(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to write personal archive"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> write_preamble(std::ofstream& output,
                                                const ArchivePreamble& preamble) {
    for (const auto bytes : {std::span<const std::byte>(preamble.header),
                             std::span<const std::byte>(preamble.envelope),
                             std::span<const std::byte>(preamble.metadata.ciphertext)}) {
        auto written = write_bytes(output, bytes);
        if (!written) {
            return written;
        }
    }
    const bool encrypted =
        (preamble.logical_header.flags & archive::kBackupFlagEncrypted) != 0;
    if (encrypted) {
        return write_bytes(output, preamble.metadata.tag);
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> write_prepared_chunk(std::ofstream& output,
                                                      const detail::PreparedArchiveChunk& chunk) {
    auto header = archive::encode_chunk_header(chunk.header);
    if (!header) {
        return base::Result<void>::failure(header.error());
    }
    auto written = write_bytes(output, header.value());
    if (!written) {
        return written;
    }
    for (const auto& entry : chunk.entries) {
        auto encoded = archive::encode_block_entry(entry);
        if (!encoded) {
            return base::Result<void>::failure(encoded.error());
        }
        written = write_bytes(output, encoded.value());
        if (!written) {
            return written;
        }
    }
    return write_bytes(output, chunk.payload);
}

[[nodiscard]] std::filesystem::path partial_path(const std::filesystem::path& destination) {
    auto result = destination;
    result += ".partial";
    return result;
}

[[nodiscard]] std::filesystem::path archive_part_path(const std::filesystem::path& destination,
                                                      const std::uint32_t part_index) {
    if (part_index == 0) {
        return destination;
    }
    std::ostringstream suffix;
    suffix << '.' << std::setw(3) << std::setfill('0') << part_index;
    auto result = destination;
    result += suffix.str();
    return result;
}

[[nodiscard]] PartArtifact make_part_artifact(const std::filesystem::path& destination,
                                              const std::uint32_t part_index) {
    auto part_destination = archive_part_path(destination, part_index);
    return {part_destination, partial_path(part_destination)};
}

[[nodiscard]] std::uint64_t chunk_wire_size(const detail::PreparedArchiveChunk& chunk) noexcept {
    return archive::kChunkHeaderSize +
           static_cast<std::uint64_t>(chunk.entries.size()) * archive::kBlockEntrySize +
           chunk.payload.size();
}

[[nodiscard]] std::filesystem::path sidecar_path(const std::filesystem::path& destination) {
    auto result = destination;
    result += ".bhx";
    return result;
}

[[nodiscard]] base::Result<void>
validate_output_paths(const std::filesystem::path& destination,
                      const std::filesystem::path& partial,
                      const std::filesystem::path& sidecar_destination,
                      const std::filesystem::path& sidecar_partial) {
    std::error_code filesystem_error;
    if (std::filesystem::exists(destination, filesystem_error) ||
        std::filesystem::exists(partial, filesystem_error) ||
        std::filesystem::exists(sidecar_destination, filesystem_error) ||
        std::filesystem::exists(sidecar_partial, filesystem_error)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive destination already exists"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> publish_outputs(const std::vector<PartArtifact>& parts,
                                                 const std::filesystem::path& sidecar_partial,
                                                 const std::filesystem::path& sidecar_destination) {
    std::vector<std::filesystem::path> published;
    std::error_code filesystem_error;
    std::filesystem::rename(sidecar_partial, sidecar_destination, filesystem_error);
    if (filesystem_error) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to publish archive sidecar"));
    }
    published.push_back(sidecar_destination);
    for (auto part = parts.rbegin(); part != parts.rend(); ++part) {
        std::filesystem::rename(part->partial, part->destination, filesystem_error);
        if (filesystem_error) {
            for (const auto& path : published) {
                std::error_code ignored;
                std::filesystem::remove(path, ignored);
            }
            return base::Result<void>::failure(
                error(base::ErrorCode::kIoFailure, "failed to publish archive part"));
        }
        published.push_back(part->destination);
    }
    return base::Result<void>::success();
}

} // namespace

struct PersonalArchiveSession::Impl final {
    Impl(const std::string_view archive_password,
         std::unique_ptr<crypto_sodium::PayloadCipher> archive_payload_cipher,
         const bool archive_encryption_enabled)
        : password(archive_password), payload_cipher(std::move(archive_payload_cipher)),
          encryption_enabled(archive_encryption_enabled) {}

    archive::BackupHeader primary_header;
    std::filesystem::path destination;
    std::filesystem::path sidecar_destination;
    std::filesystem::path sidecar_partial;
    std::vector<PartArtifact> parts;
    std::ofstream output;
    crypto_sodium::SecureString password;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    bool encryption_enabled{false};
    std::array<std::byte, 16> file_uuid{};
    crypto_sodium::KdfParameters kdf;
    std::array<std::byte, crypto_sodium::kMetadataSaltSize> salt{};
    std::uint32_t block_size{0};
    std::uint64_t split_size_bytes{0};
    std::uint64_t next_archive_chunk_index{0};
    std::uint64_t total_block_count{0};
    std::uint64_t total_payload_size{0};
    std::uint64_t current_part_chunk_count{0};
    std::vector<SourceWriteState> sources;
    std::size_t next_source_position{0};
    bool incremental{false};
    bool complete{false};

    [[nodiscard]] SourceWriteState* current_source(const std::uint32_t source_index) noexcept {
        while (next_source_position < sources.size() &&
               sources[next_source_position].next_logical_offset ==
                   sources[next_source_position].logical_size) {
            ++next_source_position;
        }
        if (next_source_position >= sources.size() ||
            sources[next_source_position].source_index != source_index) {
            return nullptr;
        }
        return &sources[next_source_position];
    }

    [[nodiscard]] base::Result<void> open_next_part() {
        if (parts.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kInsufficientSpace, "archive has too many split parts"));
        }
        const auto part_index = static_cast<std::uint32_t>(parts.size());
        auto artifact = make_part_artifact(destination, part_index);
        std::error_code filesystem_error;
        if (std::filesystem::exists(artifact.destination, filesystem_error) ||
            std::filesystem::exists(artifact.partial, filesystem_error)) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kConflict, "archive split part already exists"));
        }
        auto continuation = primary_header;
        continuation.split_part_index = part_index;
        continuation.cbor_size = 0;
        continuation.first_chunk_offset = archive::kBackupHeaderSize;
        auto encoded = archive::encode_backup_header(continuation);
        if (!encoded) {
            return base::Result<void>::failure(encoded.error());
        }
        output.close();
        parts.push_back(std::move(artifact));
        output.open(parts.back().partial, std::ios::binary | std::ios::trunc);
        auto written = write_bytes(output, encoded.value());
        if (!output || !written) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kIoFailure, "failed to create archive split part"));
        }
        current_part_chunk_count = 0;
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<void> rotate_if_needed(const detail::PreparedArchiveChunk& chunk) {
        if (split_size_bytes == 0 || current_part_chunk_count == 0) {
            return base::Result<void>::success();
        }
        const auto position = output.tellp();
        if (position < 0) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kIoFailure, "failed to determine archive part size"));
        }
        const auto current_size = static_cast<std::uint64_t>(position);
        const auto wire_size = chunk_wire_size(chunk);
        const auto reserve = wire_size + archive::kBackupFooterSize;
        if (current_size <= split_size_bytes && reserve <= split_size_bytes - current_size) {
            return base::Result<void>::success();
        }
        return open_next_part();
    }

    [[nodiscard]] base::Result<void> persist_chunks(detail::PreparedArchiveInput& prepared) {
        for (auto& chunk : prepared.chunks) {
            if (encryption_enabled) {
                if (payload_cipher == nullptr) {
                    return base::Result<void>::failure(
                        error(base::ErrorCode::kInternal, "payload cipher is missing"));
                }
                auto protected_payload = detail::protect_archive_chunk(chunk, *payload_cipher);
                if (!protected_payload) {
                    return protected_payload;
                }
            } else {
                // Wire format rejects an all-zero payload_nonce even for plaintext archives.
                // Still mint a non-zero nonce; leave tag empty and payload uncompressed plaintext.
                auto nonce = crypto_sodium::create_payload_nonce();
                if (!nonce) {
                    return base::Result<void>::failure(nonce.error());
                }
                chunk.header.payload_nonce = nonce.value();
                chunk.header.payload_authentication_tag.fill(std::byte{0});
            }
            auto rotated = rotate_if_needed(chunk);
            if (!rotated) {
                return rotated;
            }
            auto written = write_prepared_chunk(output, chunk);
            if (!written) {
                return written;
            }
            ++next_archive_chunk_index;
            ++current_part_chunk_count;
            total_payload_size += chunk.payload.size();
        }
        return base::Result<void>::success();
    }
};

PersonalArchiveSession::PersonalArchiveSession(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveSession::~PersonalArchiveSession() { abort(); }

base::Result<std::unique_ptr<PersonalArchiveSession>>
PersonalArchiveSession::create(const ArchiveCreateRequest& request) {
    auto validation = validate_create_request(request);
    if (!validation) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(validation.error());
    }
    const auto partial = partial_path(request.destination);
    const auto sidecar_destination = sidecar_path(request.destination);
    const auto sidecar_partial = partial_path(sidecar_destination);
    auto available =
        validate_output_paths(request.destination, partial, sidecar_destination, sidecar_partial);
    if (!available) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(available.error());
    }
    const auto& baseline_volume = request.manifest.volumes.front();
    auto baseline = load_incremental_baseline(request, baseline_volume);
    if (!baseline) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(baseline.error());
    }
    auto baseline_value = std::move(baseline).value();
    auto preamble = prepare_preamble(request, baseline_value);
    if (!preamble) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(preamble.error());
    }
    auto payload_cipher = create_payload_cipher(request, preamble.value());
    if (!payload_cipher) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(
            payload_cipher.error());
    }
    auto implementation = std::make_unique<Impl>(
        request.password, std::move(payload_cipher).value(), request.encryption_enabled);
    implementation->primary_header = preamble.value().logical_header;
    implementation->destination = request.destination;
    implementation->sidecar_destination = sidecar_destination;
    implementation->sidecar_partial = sidecar_partial;
    implementation->file_uuid = request.file_uuid;
    implementation->kdf = preamble.value().metadata.kdf;
    implementation->salt = preamble.value().metadata.salt;
    implementation->block_size = request.block_size;
    implementation->split_size_bytes = request.split_size_bytes;
    implementation->incremental = baseline_value.has_value();
    implementation->sources.reserve(request.manifest.volumes.size());
    for (const auto& volume : request.manifest.volumes) {
        SourceWriteState source;
        source.source_index = volume.volume_index;
        source.logical_size = volume.total_size;
        implementation->sources.push_back(std::move(source));
    }
    if (baseline_value.has_value()) {
        implementation->sources.front().baseline_records =
            std::move(baseline_value).value().records;
    }
    implementation->parts.push_back({request.destination, partial});
    implementation->output.open(partial, std::ios::binary | std::ios::trunc);
    if (!implementation->output || !write_preamble(implementation->output, preamble.value())) {
        std::error_code filesystem_error;
        implementation->output.close();
        std::filesystem::remove(partial, filesystem_error);
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to create personal archive"));
    }
    return base::Result<std::unique_ptr<PersonalArchiveSession>>::success(
        std::unique_ptr<PersonalArchiveSession>(
            new PersonalArchiveSession(std::move(implementation))));
}

base::Result<void> PersonalArchiveSession::write_chunk(const ports::ChunkWriteRequest& request,
                                                       const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive chunk descriptor is invalid"));
    }
    auto* source = implementation_->current_source(request.descriptor.source_index);
    if (source == nullptr ||
        request.descriptor.chunk_index != source->next_input_chunk_index ||
        request.descriptor.logical_offset != source->next_logical_offset ||
        request.descriptor.logical_offset % implementation_->block_size != 0 ||
        request.descriptor.logical_size != request.payload.size() ||
        request.descriptor.logical_offset > source->logical_size ||
        request.descriptor.logical_size >
            source->logical_size - request.descriptor.logical_offset) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive chunk descriptor is invalid"));
    }
    const detail::ChunkPreparationRequest preparation{
        request,
        source->baseline_records,
        implementation_->block_size,
        source->source_index,
        implementation_->next_archive_chunk_index,
        implementation_->incremental,
    };
    auto prepared = detail::prepare_archive_chunks(preparation);
    if (!prepared) {
        return base::Result<void>::failure(prepared.error());
    }
    auto persisted = implementation_->persist_chunks(prepared.value());
    if (!persisted) {
        return persisted;
    }
    ++source->next_input_chunk_index;
    source->next_logical_offset += request.descriptor.logical_size;
    implementation_->total_block_count += prepared.value().sidecar_records.size();
    source->sidecar_records.insert(source->sidecar_records.end(),
                                   prepared.value().sidecar_records.begin(),
                                   prepared.value().sidecar_records.end());
    return base::Result<void>::success();
}

base::Result<void> PersonalArchiveSession::commit(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    const bool all_sources_complete =
        implementation_ && std::ranges::all_of(implementation_->sources, [](const auto& source) {
            return source.next_input_chunk_index > 0 &&
                   source.next_logical_offset == source.logical_size;
        });
    if (!implementation_ || implementation_->complete || !all_sources_complete) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive session cannot be committed"));
    }
    const auto footer_offset = implementation_->output.tellp();
    if (footer_offset < 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to determine archive size"));
    }
    archive::BackupFooter footer;
    footer.chunk_count = implementation_->next_archive_chunk_index;
    footer.total_block_count = implementation_->total_block_count;
    footer.total_payload_size = implementation_->total_payload_size;
    footer.file_size = static_cast<std::uint64_t>(footer_offset) + archive::kBackupFooterSize;
    auto encoded = archive::encode_backup_footer(footer);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    auto written = write_bytes(implementation_->output, encoded.value());
    implementation_->output.flush();
    if (!written || !implementation_->output) {
        abort();
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to finalize personal archive"));
    }
    archive::SidecarPayload sidecar_payload;
    sidecar_payload.volumes.reserve(implementation_->sources.size());
    for (const auto& source : implementation_->sources) {
        sidecar_payload.volumes.push_back({source.source_index, source.sidecar_records});
    }
    const detail::SidecarWriteRequest sidecar_request{
        implementation_->sidecar_partial,
        implementation_->password.view(),
        implementation_->encryption_enabled,
        implementation_->file_uuid,
        implementation_->block_size,
        sidecar_payload,
        implementation_->kdf,
        implementation_->salt,
    };
    auto sidecar_written = detail::write_sidecar(sidecar_request);
    if (!sidecar_written) {
        const auto& failure = sidecar_written.error();
        abort();
        return base::Result<void>::failure(failure);
    }
    implementation_->output.close();
    auto published = publish_outputs(implementation_->parts, implementation_->sidecar_partial,
                                     implementation_->sidecar_destination);
    if (!published) {
        const auto& failure = published.error();
        abort();
        return base::Result<void>::failure(failure);
    }
    implementation_->complete = true;
    implementation_->password.clear();
    return base::Result<void>::success();
}

void PersonalArchiveSession::abort() noexcept {
    if (!implementation_ || implementation_->complete) {
        return;
    }
    try {
        implementation_->output.close();
    } catch (...) {
        // Cleanup must remain noexcept; remove the owned partial file below.
        static_cast<void>(0);
    }
    std::error_code ignored;
    for (const auto& part : implementation_->parts) {
        std::filesystem::remove(part.partial, ignored);
    }
    std::filesystem::remove(implementation_->sidecar_partial, ignored);
    implementation_->password.clear();
    implementation_->complete = true;
}

} // namespace aegra::adapters::personal_archive
