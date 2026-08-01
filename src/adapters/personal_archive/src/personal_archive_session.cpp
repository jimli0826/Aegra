#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/crypto_sodium/secure_string.h"
#include "aegra/base/error.h"
#include "aegra/format/manifest_codec.h"
#include "aegra/format/personal_archive.h"

#include "personal_archive_sidecar_io.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;

inline constexpr std::uint64_t kMaximumMetadataSize = 64ULL * 1024ULL * 1024ULL;

struct ArchivePreamble final {
    archive::EncodedBackupHeader header;
    archive::EncodedMetadataEnvelopeHeader envelope;
    crypto_sodium::ProtectedMetadata metadata;
};

struct PreparedChunk final {
    archive::ChunkHeader header;
    std::vector<archive::BlockEntry> entries;
    std::vector<std::byte> payload;
    std::vector<archive::SidecarRecord> sidecar_records;
};

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] const char* as_chars(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<const char*>(value);
}

[[nodiscard]] base::Result<void> validate_create_request(const ArchiveCreateRequest& request) {
    if (request.destination.empty() || request.password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive path and password are required"));
    }
    if (request.block_size == 0 || request.chunk_size < request.block_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive block and chunk sizes are invalid"));
    }
    if (request.manifest.volumes.size() != 1) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument,
                  "personal archive MVP requires exactly one source volume"));
    }
    const auto volume =
        std::find_if(request.manifest.volumes.begin(), request.manifest.volumes.end(),
                     [&request](const format::Volume& candidate) {
                         return candidate.volume_index == request.source_index;
                     });
    if (volume == request.manifest.volumes.end()) {
        return base::Result<void>::failure(error(base::ErrorCode::kInvalidArgument,
                                                 "archive source volume is absent from manifest"));
    }
    return format::validate_manifest(request.manifest);
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

[[nodiscard]] archive::BackupHeader make_header(const ArchiveCreateRequest& request,
                                                const std::uint64_t metadata_size) {
    archive::BackupHeader header;
    header.file_uuid = request.file_uuid;
    header.backup_set_uuid = request.backup_set_uuid;
    header.block_size = request.block_size;
    header.cbor_size =
        archive::kMetadataEnvelopeHeaderSize + metadata_size + crypto_sodium::kMetadataTagSize;
    header.first_chunk_offset = header.cbor_offset + header.cbor_size;
    header.default_chunk_size = request.chunk_size;
    header.compression_method = archive::CompressionMethod::kZstandard;
    return header;
}

[[nodiscard]] base::Result<ArchivePreamble> prepare_preamble(const ArchiveCreateRequest& request) {
    auto cbor = format::encode_manifest_cbor(request.manifest);
    if (!cbor) {
        return base::Result<ArchivePreamble>::failure(cbor.error());
    }
    if (cbor.value().size() > kMaximumMetadataSize) {
        return base::Result<ArchivePreamble>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive metadata exceeds product limit"));
    }
    const crypto_sodium::KdfParameters kdf{request.kdf_parameters.opslimit,
                                           request.kdf_parameters.memlimit_bytes,
                                           crypto_sodium::kKdfParametersVersion};
    auto context = crypto_sodium::create_metadata_protection_context(kdf);
    if (!context) {
        return base::Result<ArchivePreamble>::failure(context.error());
    }
    auto logical_header = make_header(request, cbor.value().size());
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
        {header.value(), envelope.value(), std::move(metadata).value()});
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
                             std::span<const std::byte>(preamble.metadata.ciphertext),
                             std::span<const std::byte>(preamble.metadata.tag)}) {
        auto written = write_bytes(output, bytes);
        if (!written) {
            return written;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool is_zero_block(const std::span<const std::byte> block) {
    return std::all_of(block.begin(), block.end(),
                       [](const std::byte value) { return value == std::byte{0}; });
}

void append_zero_block(PreparedChunk& chunk, const std::uint64_t logical_block_index) {
    if (!chunk.entries.empty()) {
        auto& previous = chunk.entries.back();
        if (previous.flags == archive::kBlockFlagZero &&
            previous.logical_block_index + previous.logical_size == logical_block_index &&
            previous.logical_size < (std::numeric_limits<std::uint32_t>::max)()) {
            ++previous.logical_size;
            chunk.sidecar_records.push_back({archive::SidecarBlockState::kZero, {}});
            return;
        }
    }
    archive::BlockEntry entry;
    entry.logical_block_index = logical_block_index;
    entry.flags = archive::kBlockFlagZero;
    entry.logical_size = 1;
    chunk.entries.push_back(entry);
    chunk.sidecar_records.push_back({archive::SidecarBlockState::kZero, {}});
}

[[nodiscard]] base::Result<void> append_data_block(PreparedChunk& chunk,
                                                   const std::span<const std::byte> block,
                                                   const std::uint64_t logical_block_index) {
    auto compressed = compression_zstd::compress(block);
    if (!compressed) {
        return base::Result<void>::failure(compressed.error());
    }
    const bool use_compressed = compressed.value().size() < block.size();
    const auto stored = use_compressed ? std::span<const std::byte>(compressed.value()) : block;
    if (stored.size() > std::numeric_limits<std::uint32_t>::max()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive block exceeds format limit"));
    }
    archive::BlockEntry entry;
    entry.logical_block_index = logical_block_index;
    entry.data_offset_or_reference = chunk.payload.size();
    entry.stored_size = static_cast<std::uint32_t>(stored.size());
    entry.logical_size = static_cast<std::uint32_t>(use_compressed ? stored.size() : block.size());
    entry.flags = use_compressed ? archive::kBlockFlagCompressed : archive::kBlockFlagRaw;
    chunk.entries.push_back(entry);
    chunk.payload.insert(chunk.payload.end(), stored.begin(), stored.end());
    auto digest = crypto_sodium::sha256(block);
    if (!digest) {
        return base::Result<void>::failure(digest.error());
    }
    chunk.sidecar_records.push_back({archive::SidecarBlockState::kData, digest.value()});
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> append_block(PreparedChunk& chunk,
                                              const std::span<const std::byte> block,
                                              const std::uint64_t logical_block_index) {
    if (is_zero_block(block)) {
        append_zero_block(chunk, logical_block_index);
        return base::Result<void>::success();
    }
    return append_data_block(chunk, block, logical_block_index);
}

[[nodiscard]] base::Result<PreparedChunk> prepare_chunk(const ports::ChunkWriteRequest& request,
                                                        const std::uint32_t block_size) {
    PreparedChunk result;
    result.header.chunk_index = request.descriptor.chunk_index;
    std::size_t offset = 0;
    while (offset < request.payload.size()) {
        const auto remaining = request.payload.size() - offset;
        const auto current_size = (std::min)(remaining, static_cast<std::size_t>(block_size));
        const auto logical_offset = request.descriptor.logical_offset + offset;
        auto appended = append_block(result, request.payload.subspan(offset, current_size),
                                     logical_offset / block_size);
        if (!appended) {
            return base::Result<PreparedChunk>::failure(appended.error());
        }
        offset += current_size;
    }
    result.header.block_entry_count = static_cast<std::uint32_t>(result.entries.size());
    result.header.payload_size = result.payload.size();
    return base::Result<PreparedChunk>::success(std::move(result));
}

[[nodiscard]] base::Result<void> write_prepared_chunk(std::ofstream& output,
                                                      const PreparedChunk& chunk) {
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

[[nodiscard]] std::filesystem::path sidecar_path(const std::filesystem::path& destination) {
    auto result = destination;
    result += ".bhx";
    return result;
}

[[nodiscard]] base::Result<void> publish_outputs(const std::filesystem::path& archive_partial,
                                                 const std::filesystem::path& archive_destination,
                                                 const std::filesystem::path& sidecar_partial,
                                                 const std::filesystem::path& sidecar_destination) {
    std::error_code filesystem_error;
    std::filesystem::rename(archive_partial, archive_destination, filesystem_error);
    if (filesystem_error) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to publish personal archive"));
    }
    std::filesystem::rename(sidecar_partial, sidecar_destination, filesystem_error);
    if (!filesystem_error) {
        return base::Result<void>::success();
    }
    std::error_code ignored;
    std::filesystem::remove(archive_destination, ignored);
    std::filesystem::remove(sidecar_partial, ignored);
    return base::Result<void>::failure(
        error(base::ErrorCode::kIoFailure, "failed to publish archive sidecar"));
}

} // namespace

struct PersonalArchiveSession::Impl final {
    explicit Impl(const std::string_view archive_password) : password(archive_password) {}

    std::filesystem::path destination;
    std::filesystem::path partial;
    std::filesystem::path sidecar_destination;
    std::filesystem::path sidecar_partial;
    std::ofstream output;
    crypto_sodium::SecureString password;
    std::array<std::byte, 16> file_uuid{};
    crypto_sodium::KdfParameters kdf;
    std::array<std::byte, crypto_sodium::kMetadataSaltSize> salt{};
    std::uint32_t block_size{0};
    std::uint32_t source_index{0};
    std::uint64_t logical_size{0};
    std::uint64_t next_logical_offset{0};
    std::uint64_t next_chunk_index{0};
    std::uint64_t total_block_count{0};
    std::uint64_t total_payload_size{0};
    std::vector<archive::SidecarRecord> sidecar_records;
    bool complete{false};
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
    std::error_code filesystem_error;
    const auto partial = partial_path(request.destination);
    const auto sidecar_destination = sidecar_path(request.destination);
    const auto sidecar_partial = partial_path(sidecar_destination);
    if (std::filesystem::exists(request.destination, filesystem_error) ||
        std::filesystem::exists(partial, filesystem_error) ||
        std::filesystem::exists(sidecar_destination, filesystem_error) ||
        std::filesystem::exists(sidecar_partial, filesystem_error)) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(
            error(base::ErrorCode::kConflict, "archive destination already exists"));
    }
    auto preamble = prepare_preamble(request);
    if (!preamble) {
        return base::Result<std::unique_ptr<PersonalArchiveSession>>::failure(preamble.error());
    }
    auto implementation = std::make_unique<Impl>(request.password);
    implementation->destination = request.destination;
    implementation->partial = partial;
    implementation->sidecar_destination = sidecar_destination;
    implementation->sidecar_partial = sidecar_partial;
    implementation->file_uuid = request.file_uuid;
    implementation->kdf = preamble.value().metadata.kdf;
    implementation->salt = preamble.value().metadata.salt;
    implementation->block_size = request.block_size;
    implementation->source_index = request.source_index;
    const auto volume =
        std::find_if(request.manifest.volumes.begin(), request.manifest.volumes.end(),
                     [&request](const format::Volume& candidate) {
                         return candidate.volume_index == request.source_index;
                     });
    implementation->logical_size = volume->total_size;
    implementation->output.open(partial, std::ios::binary | std::ios::trunc);
    if (!implementation->output || !write_preamble(implementation->output, preamble.value())) {
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
    if (!implementation_ || implementation_->complete ||
        request.descriptor.chunk_index != implementation_->next_chunk_index ||
        request.descriptor.logical_offset != implementation_->next_logical_offset ||
        request.descriptor.logical_offset % implementation_->block_size != 0 ||
        request.descriptor.logical_size != request.payload.size() ||
        request.descriptor.logical_offset > implementation_->logical_size ||
        request.descriptor.logical_size >
            implementation_->logical_size - request.descriptor.logical_offset) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive chunk descriptor is invalid"));
    }
    auto prepared = prepare_chunk(request, implementation_->block_size);
    if (!prepared) {
        return base::Result<void>::failure(prepared.error());
    }
    prepared.value().header.source_index = implementation_->source_index;
    auto written = write_prepared_chunk(implementation_->output, prepared.value());
    if (!written) {
        return written;
    }
    ++implementation_->next_chunk_index;
    implementation_->next_logical_offset += request.descriptor.logical_size;
    implementation_->total_block_count += prepared.value().sidecar_records.size();
    implementation_->total_payload_size += prepared.value().payload.size();
    implementation_->sidecar_records.insert(implementation_->sidecar_records.end(),
                                            prepared.value().sidecar_records.begin(),
                                            prepared.value().sidecar_records.end());
    return base::Result<void>::success();
}

base::Result<void> PersonalArchiveSession::commit(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || implementation_->next_chunk_index == 0 ||
        implementation_->next_logical_offset != implementation_->logical_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive session cannot be committed"));
    }
    const auto footer_offset = implementation_->output.tellp();
    if (footer_offset < 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to determine archive size"));
    }
    archive::BackupFooter footer;
    footer.chunk_count = implementation_->next_chunk_index;
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
    const detail::SidecarWriteRequest sidecar_request{
        implementation_->sidecar_partial,
        implementation_->password.view(),
        implementation_->file_uuid,
        implementation_->block_size,
        implementation_->source_index,
        implementation_->sidecar_records,
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
    auto published =
        publish_outputs(implementation_->partial, implementation_->destination,
                        implementation_->sidecar_partial, implementation_->sidecar_destination);
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
    std::filesystem::remove(implementation_->partial, ignored);
    std::filesystem::remove(implementation_->sidecar_partial, ignored);
    implementation_->password.clear();
    implementation_->complete = true;
}

} // namespace aegra::adapters::personal_archive
