#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/adapters/crypto_sodium/secure_string.h"
#include "aegra/base/error.h"
#include "aegra/format/file_index.h"
#include "aegra/format/manifest_codec.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;
namespace index = format::file_index;

inline constexpr std::uint64_t kMaximumMetadataSize = 64ULL * 1024ULL * 1024ULL;

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] const char* as_chars(const std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<const char*>(value);
}

[[nodiscard]] char* as_mutable_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<void> write_bytes(std::ofstream& output,
                                             const std::span<const std::byte> bytes) {
    output.write(as_chars(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!output) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to write file archive"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::filesystem::path partial_path(const std::filesystem::path& destination) {
    auto result = destination;
    result += ".partial";
    return result;
}

[[nodiscard]] std::vector<std::byte>
make_metadata_aad(const archive::EncodedBackupHeader& header,
                  const archive::EncodedMetadataEnvelopeHeader& envelope) {
    std::vector<std::byte> result;
    result.reserve(header.size() + envelope.size());
    result.insert(result.end(), header.begin(), header.end());
    result.insert(result.end(), envelope.begin(), envelope.end());
    return result;
}

[[nodiscard]] base::Result<void> validate_file_create(const FileArchiveCreateRequest& request) {
    if (request.destination.empty() || request.index_spool_directory.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive paths are required"));
    }
    if (request.encryption_enabled && request.password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "encrypted file archive requires a password"));
    }
    if (!request.encryption_enabled && !request.password.empty()) {
        return base::Result<void>::failure(error(
            base::ErrorCode::kInvalidArgument, "unencrypted file archive must not supply a password"));
    }
    if (request.block_size < archive::kMinimumFileBlockSizeBytes ||
        request.block_size % archive::kFileBlockSizeAlignment != 0 ||
        request.chunk_size < request.block_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive block size is invalid"));
    }
    if (request.manifest.content_kind != format::kManifestContentKindFileSet) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive requires file_set manifest"));
    }
    return format::validate_manifest(request.manifest);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
make_file_chunk_aad(const archive::EncodedBackupHeader& part_header, const std::uint64_t body_size,
                    const archive::FileStreamChunkHeader& header,
                    const std::span<const archive::BlockEntry> entries) {
    auto prefix =
        archive::encode_archive_record_prefix(archive::make_file_stream_chunk_record_prefix(body_size));
    auto authenticated = header;
    authenticated.payload_authentication_tag.fill(std::byte{0});
    auto encoded_header = archive::encode_file_stream_chunk_header(authenticated);
    if (!prefix || !encoded_header) {
        return base::Result<std::vector<std::byte>>::failure(!prefix ? prefix.error()
                                                                     : encoded_header.error());
    }
    std::vector<std::byte> aad;
    aad.reserve(part_header.size() + prefix.value().size() + encoded_header.value().size() +
                entries.size() * archive::kBlockEntrySize);
    aad.insert(aad.end(), part_header.begin(), part_header.end());
    aad.insert(aad.end(), prefix.value().begin(), prefix.value().end());
    aad.insert(aad.end(), encoded_header.value().begin(), encoded_header.value().end());
    for (const auto& entry : entries) {
        auto encoded = archive::encode_block_entry(entry);
        if (!encoded) {
            return base::Result<std::vector<std::byte>>::failure(encoded.error());
        }
        aad.insert(aad.end(), encoded.value().begin(), encoded.value().end());
    }
    return base::Result<std::vector<std::byte>>::success(std::move(aad));
}

[[nodiscard]] base::Result<std::vector<std::byte>>
make_index_page_aad(const archive::EncodedBackupHeader& part_header, const std::uint64_t body_size,
                    const archive::FileIndexPageHeader& header) {
    auto prefix =
        archive::encode_archive_record_prefix(archive::make_file_index_page_record_prefix(body_size));
    auto authenticated = header;
    authenticated.authentication_tag.fill(std::byte{0});
    authenticated.content_digest.fill(std::byte{0});
    auto encoded_header = archive::encode_file_index_page_header(authenticated);
    if (!prefix || !encoded_header) {
        return base::Result<std::vector<std::byte>>::failure(!prefix ? prefix.error()
                                                                     : encoded_header.error());
    }
    std::vector<std::byte> aad;
    aad.reserve(part_header.size() + prefix.value().size() + encoded_header.value().size());
    aad.insert(aad.end(), part_header.begin(), part_header.end());
    aad.insert(aad.end(), prefix.value().begin(), prefix.value().end());
    aad.insert(aad.end(), encoded_header.value().begin(), encoded_header.value().end());
    return base::Result<std::vector<std::byte>>::success(std::move(aad));
}

[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
load_spool_entries(std::ifstream& spool_input, const std::uint64_t entry_count) {
    std::vector<contracts::FileEntryDesc> entries;
    entries.reserve(static_cast<std::size_t>(entry_count));
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        std::array<std::byte, 4> size_bytes{};
        spool_input.read(as_mutable_chars(size_bytes.data()), 4);
        if (!spool_input) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                error(base::ErrorCode::kCorruptData, "index spool is truncated"));
        }
        std::uint32_t size = 0;
        for (std::size_t i = 0; i < 4; ++i) {
            size |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(size_bytes[i]))
                    << (i * 8U);
        }
        std::vector<std::byte> encoded(size);
        if (size != 0) {
            spool_input.read(as_mutable_chars(encoded.data()),
                             static_cast<std::streamsize>(size));
        }
        if (!spool_input) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                error(base::ErrorCode::kCorruptData, "index spool entry is truncated"));
        }
        auto entry = index::decode_leaf_entry_cbor(encoded);
        if (!entry) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(entry.error());
        }
        entries.push_back(std::move(entry).value());
    }
    std::ranges::sort(entries, [](const contracts::FileEntryDesc& left,
                                  const contracts::FileEntryDesc& right) {
        auto left_key = index::make_index_key(left);
        auto right_key = index::make_index_key(right);
        if (!left_key || !right_key) {
            return left.entry_id < right.entry_id;
        }
        return index::compare_index_keys(left_key.value(), right_key.value()) < 0;
    });
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(entries));
}

struct PreparedIndexPage final {
    archive::FileIndexPageHeader header;
    std::vector<std::byte> ciphertext;
};

[[nodiscard]] base::Result<PreparedIndexPage>
protect_index_page(PreparedIndexPage prepared, const bool encryption_enabled,
                   crypto_sodium::PayloadCipher* payload_cipher,
                   const archive::EncodedBackupHeader& part_header) {
    if (prepared.ciphertext.empty() ||
        prepared.ciphertext.size() > archive::kMaximumIndexPagePlainBytes) {
        return base::Result<PreparedIndexPage>::failure(error(
            base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
    }
    prepared.header.plain_size = static_cast<std::uint32_t>(prepared.ciphertext.size());
    prepared.header.encoded_size = prepared.header.plain_size;
    auto nonce = crypto_sodium::create_payload_nonce();
    if (!nonce) {
        return base::Result<PreparedIndexPage>::failure(nonce.error());
    }
    prepared.header.nonce = nonce.value();
    if (!encryption_enabled) {
        return base::Result<PreparedIndexPage>::success(std::move(prepared));
    }
    if (payload_cipher == nullptr) {
        return base::Result<PreparedIndexPage>::failure(
            error(base::ErrorCode::kInternal, "file archive payload cipher is missing"));
    }
    auto aad = make_index_page_aad(part_header, prepared.ciphertext.size(), prepared.header);
    if (!aad) {
        return base::Result<PreparedIndexPage>::failure(aad.error());
    }
    auto protected_page =
        payload_cipher->protect(prepared.ciphertext, aad.value(), prepared.header.nonce);
    if (!protected_page) {
        return base::Result<PreparedIndexPage>::failure(protected_page.error());
    }
    prepared.ciphertext = std::move(protected_page.value().ciphertext);
    prepared.header.authentication_tag = protected_page.value().tag;
    prepared.header.encoded_size = static_cast<std::uint32_t>(prepared.ciphertext.size());
    return base::Result<PreparedIndexPage>::success(std::move(prepared));
}

[[nodiscard]] base::Result<PreparedIndexPage>
prepare_leaf_index_page(const std::vector<contracts::FileEntryDesc>& entries,
                        const std::uint64_t page_id, const bool encryption_enabled,
                        crypto_sodium::PayloadCipher* payload_cipher,
                        const archive::EncodedBackupHeader& part_header) {
    index::LeafPageBody leaf;
    leaf.entries = entries;
    auto plain = index::encode_leaf_page_cbor(leaf);
    if (!plain) {
        return base::Result<PreparedIndexPage>::failure(plain.error());
    }
    auto digest = crypto_sodium::sha256(plain.value());
    if (!digest) {
        return base::Result<PreparedIndexPage>::failure(digest.error());
    }
    PreparedIndexPage prepared;
    prepared.header.page_kind = archive::kIndexPageLeaf;
    prepared.header.page_id = page_id;
    prepared.header.entry_count = static_cast<std::uint32_t>(entries.size());
    prepared.header.content_digest = digest.value();
    prepared.ciphertext = std::move(plain).value();
    return protect_index_page(std::move(prepared), encryption_enabled, payload_cipher, part_header);
}

[[nodiscard]] base::Result<PreparedIndexPage>
prepare_internal_index_page(const index::InternalPageBody& body, const std::uint64_t page_id,
                            const bool encryption_enabled,
                            crypto_sodium::PayloadCipher* payload_cipher,
                            const archive::EncodedBackupHeader& part_header) {
    auto plain = index::encode_internal_page_cbor(body);
    if (!plain) {
        return base::Result<PreparedIndexPage>::failure(plain.error());
    }
    auto digest = crypto_sodium::sha256(plain.value());
    if (!digest) {
        return base::Result<PreparedIndexPage>::failure(digest.error());
    }
    PreparedIndexPage prepared;
    prepared.header.page_kind = archive::kIndexPageInternal;
    prepared.header.page_id = page_id;
    prepared.header.entry_count = static_cast<std::uint32_t>(body.keys.size());
    prepared.header.content_digest = digest.value();
    prepared.ciphertext = std::move(plain).value();
    return protect_index_page(std::move(prepared), encryption_enabled, payload_cipher, part_header);
}

[[nodiscard]] base::Result<bool>
leaf_page_fits(const std::vector<contracts::FileEntryDesc>& entries) {
    if (entries.empty() || entries.size() > index::kMaximumLeafEntriesPerPage) {
        return base::Result<bool>::success(false);
    }
    index::LeafPageBody leaf;
    leaf.entries = entries;
    auto plain = index::encode_leaf_page_cbor(leaf);
    if (!plain) {
        return base::Result<bool>::failure(plain.error());
    }
    return base::Result<bool>::success(plain.value().size() <=
                                       archive::kMaximumIndexPagePlainBytes);
}

[[nodiscard]] base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>
pack_leaf_pages(const std::vector<contracts::FileEntryDesc>& entries) {
    std::vector<std::vector<contracts::FileEntryDesc>> pages;
    std::vector<contracts::FileEntryDesc> current;
    current.reserve((std::min)(entries.size(),
                               static_cast<std::size_t>(index::kMaximumLeafEntriesPerPage)));
    for (const auto& entry : entries) {
        auto trial = current;
        trial.push_back(entry);
        auto fits = leaf_page_fits(trial);
        if (!fits) {
            return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::failure(
                fits.error());
        }
        if (fits.value()) {
            current = std::move(trial);
            continue;
        }
        if (current.empty()) {
            return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
        pages.push_back(std::move(current));
        current.clear();
        current.push_back(entry);
        auto single = leaf_page_fits(current);
        if (!single) {
            return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::failure(
                single.error());
        }
        if (!single.value()) {
            return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
    }
    if (!current.empty()) {
        pages.push_back(std::move(current));
    }
    if (pages.empty()) {
        return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive requires at least one entry"));
    }
    return base::Result<std::vector<std::vector<contracts::FileEntryDesc>>>::success(
        std::move(pages));
}

} // namespace

struct PersonalFileArchiveSession::Impl final {
    explicit Impl(const std::string_view archive_password, const bool archive_encryption_enabled)
        : password(archive_password), encryption_enabled(archive_encryption_enabled) {}

    std::filesystem::path destination;
    std::filesystem::path partial;
    std::filesystem::path spool_path;
    std::ofstream output;
    std::ofstream spool;
    crypto_sodium::SecureString password;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    std::unique_ptr<crypto_sodium::PayloadCipher> index_cipher;
    bool encryption_enabled{true};
    archive::EncodedBackupHeader part_header{};
    std::array<std::byte, 16> file_uuid{};
    std::uint32_t block_size{0};
    std::uint64_t next_chunk_index{0};
    std::uint64_t total_block_entries{0};
    std::uint64_t total_payload_size{0};
    std::uint64_t logical_bytes{0};
    std::uint64_t entry_count{0};
    std::uint64_t stream_count{0};
    std::uint64_t next_page_id{1};
    bool finalized{false};
    bool complete{false};

    [[nodiscard]] base::Result<void> write_index_page(const archive::FileIndexPageHeader& header,
                                                      const std::span<const std::byte> ciphertext) {
        const auto body_size = ciphertext.size();
        auto prefix = archive::encode_archive_record_prefix(
            archive::make_file_index_page_record_prefix(body_size));
        auto encoded_header = archive::encode_file_index_page_header(header);
        if (!prefix || !encoded_header) {
            return base::Result<void>::failure(!prefix ? prefix.error() : encoded_header.error());
        }
        auto written = write_bytes(output, prefix.value());
        if (!written) {
            return written;
        }
        written = write_bytes(output, encoded_header.value());
        if (!written) {
            return written;
        }
        return write_bytes(output, ciphertext);
    }
};

PersonalFileArchiveSession::PersonalFileArchiveSession(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalFileArchiveSession::~PersonalFileArchiveSession() { abort(); }

base::Result<std::unique_ptr<PersonalFileArchiveSession>>
PersonalFileArchiveSession::create(const FileArchiveCreateRequest& request) {
    auto validation = validate_file_create(request);
    if (!validation) {
        return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
            validation.error());
    }
    const auto partial = partial_path(request.destination);
    std::error_code filesystem_error;
    if (std::filesystem::exists(request.destination, filesystem_error) ||
        std::filesystem::exists(partial, filesystem_error)) {
        return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
            error(base::ErrorCode::kConflict, "file archive destination already exists"));
    }
    std::filesystem::create_directories(request.index_spool_directory, filesystem_error);
    if (filesystem_error) {
        return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to create index spool directory"));
    }
    auto cbor = format::encode_manifest_cbor(request.manifest);
    if (!cbor || cbor.value().size() > kMaximumMetadataSize) {
        return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
            !cbor ? cbor.error()
                  : error(base::ErrorCode::kInvalidArgument, "file archive metadata exceeds limit"));
    }

    archive::BackupHeader logical_header;
    logical_header.file_uuid = request.file_uuid;
    logical_header.backup_set_uuid = request.backup_set_uuid;
    logical_header.block_size = request.block_size;
    logical_header.flags = archive::kBackupFlagFull;
    logical_header.content_kind = archive::kContentKindFileSet;
    logical_header.capability_flags = archive::kCapabilityHasFileIndex;
    logical_header.default_chunk_size = request.chunk_size;
    logical_header.compression_method = archive::CompressionMethod::kNone;
    if (request.encryption_enabled) {
        logical_header.flags |= archive::kBackupFlagEncrypted;
        logical_header.encryption_method = archive::PayloadEncryptionMethod::kXChaCha20Poly1305;
    }

    auto implementation =
        std::make_unique<Impl>(request.password, request.encryption_enabled);
    implementation->destination = request.destination;
    implementation->partial = partial;
    implementation->spool_path = request.index_spool_directory / "index.spool";
    implementation->file_uuid = request.file_uuid;
    implementation->block_size = request.block_size;

    archive::EncodedMetadataEnvelopeHeader encoded_envelope{};
    crypto_sodium::ProtectedMetadata metadata;
    if (request.encryption_enabled) {
        const crypto_sodium::KdfParameters kdf{request.kdf_parameters.opslimit,
                                               request.kdf_parameters.memlimit_bytes,
                                               crypto_sodium::kKdfParametersVersion};
        auto context = crypto_sodium::create_metadata_protection_context(kdf);
        if (!context) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                context.error());
        }
        logical_header.cbor_size = archive::kMetadataEnvelopeHeaderSize + cbor.value().size() +
                                   crypto_sodium::kMetadataTagSize;
        logical_header.first_record_offset =
            logical_header.cbor_offset + logical_header.cbor_size;
        auto header = archive::encode_backup_header(logical_header);
        archive::MetadataEnvelopeHeader envelope;
        envelope.plaintext_size = cbor.value().size();
        envelope.ciphertext_size = cbor.value().size();
        envelope.salt = context.value().salt;
        envelope.nonce = context.value().nonce;
        envelope.kdf_opslimit = context.value().kdf.opslimit;
        envelope.kdf_memlimit_bytes = context.value().kdf.memlimit_bytes;
        envelope.kdf_parameters_version = context.value().kdf.parameters_version;
        auto encoded_env = archive::encode_metadata_envelope_header(envelope);
        if (!header || !encoded_env) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                !header ? header.error() : encoded_env.error());
        }
        const auto aad = make_metadata_aad(header.value(), encoded_env.value());
        auto protected_metadata =
            crypto_sodium::protect_metadata(cbor.value(), request.password, aad, context.value());
        if (!protected_metadata) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                protected_metadata.error());
        }
        metadata = std::move(protected_metadata).value();
        encoded_envelope = encoded_env.value();
        implementation->part_header = header.value();
        auto cipher = crypto_sodium::PayloadCipher::create(request.password, metadata.kdf,
                                                           metadata.salt);
        if (!cipher) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(cipher.error());
        }
        implementation->payload_cipher = std::move(cipher).value();
        auto index_cipher = crypto_sodium::PayloadCipher::create_index_page(
            request.password, metadata.kdf, metadata.salt);
        if (!index_cipher) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                index_cipher.error());
        }
        implementation->index_cipher = std::move(index_cipher).value();
    } else {
        logical_header.cbor_size = archive::kMetadataEnvelopeHeaderSize + cbor.value().size();
        logical_header.first_record_offset =
            logical_header.cbor_offset + logical_header.cbor_size;
        logical_header.encryption_method = archive::PayloadEncryptionMethod::kNone;
        auto header = archive::encode_backup_header(logical_header);
        archive::MetadataEnvelopeHeader envelope;
        envelope.flags = 0;
        envelope.encryption_method = archive::MetadataEncryptionMethod::kNone;
        envelope.kdf_method = archive::MetadataKdfMethod::kNone;
        envelope.nonce_size = 0;
        envelope.tag_size = 0;
        envelope.plaintext_size = cbor.value().size();
        envelope.ciphertext_size = cbor.value().size();
        envelope.kdf_parameters_version = 0;
        auto encoded_env = archive::encode_metadata_envelope_header(envelope);
        if (!header || !encoded_env) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                !header ? header.error() : encoded_env.error());
        }
        metadata.ciphertext = std::move(cbor).value();
        encoded_envelope = encoded_env.value();
        implementation->part_header = header.value();
    }

    implementation->output.open(partial, std::ios::binary | std::ios::trunc);
    implementation->spool.open(implementation->spool_path, std::ios::binary | std::ios::trunc);
    if (!implementation->output || !implementation->spool) {
        return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
            error(base::ErrorCode::kIoFailure, "failed to create file archive partials"));
    }
    for (const auto bytes : {std::span<const std::byte>(implementation->part_header),
                             std::span<const std::byte>(encoded_envelope),
                             std::span<const std::byte>(metadata.ciphertext)}) {
        auto written = write_bytes(implementation->output, bytes);
        if (!written) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                written.error());
        }
    }
    if (request.encryption_enabled) {
        auto written = write_bytes(implementation->output, metadata.tag);
        if (!written) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                written.error());
        }
    }
    return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::success(
        std::unique_ptr<PersonalFileArchiveSession>(
            new PersonalFileArchiveSession(std::move(implementation))));
}

base::Result<void>
PersonalFileArchiveSession::write_entry(const contracts::FileEntryDesc& entry,
                                        const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || implementation_->finalized) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive session is not writable"));
    }
    if (implementation_->entry_count >= index::kMaximumEntries) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive entry limit exceeded"));
    }
    auto encoded = index::encode_leaf_entry_cbor(entry);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    if (encoded.value().size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file entry encoding is too large"));
    }
    const auto size = static_cast<std::uint32_t>(encoded.value().size());
    std::array<std::byte, 4> size_bytes{};
    for (std::size_t i = 0; i < 4; ++i) {
        size_bytes[i] = static_cast<std::byte>((size >> (i * 8U)) & 0xFFU);
    }
    auto written = write_bytes(implementation_->spool, size_bytes);
    if (!written) {
        return written;
    }
    written = write_bytes(implementation_->spool, encoded.value());
    if (!written) {
        return written;
    }
    ++implementation_->entry_count;
    implementation_->stream_count += entry.streams.size();
    return base::Result<void>::success();
}

base::Result<void>
PersonalFileArchiveSession::write_stream_chunk(const ports::FileChunkWriteRequest& request,
                                               const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || implementation_->finalized) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive session is not writable"));
    }
    if (request.chunk_index != implementation_->next_chunk_index || request.stream_index == 0 ||
        request.logical_size == 0 || request.logical_size > implementation_->block_size ||
        request.payload.size() != request.logical_size) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream chunk descriptor is invalid"));
    }
    archive::BlockEntry entry;
    entry.logical_block_index = request.logical_block_index;
    entry.data_offset_or_reference = 0;
    entry.stored_size = request.logical_size;
    entry.logical_size = request.logical_size;
    entry.flags = request.block_flags == 0 ? archive::kBlockFlagRaw : request.block_flags;

    archive::FileStreamChunkHeader header;
    header.chunk_index = request.chunk_index;
    header.source_type = archive::kSourceTypeFileStream;
    header.source_index = request.stream_index;
    header.block_entry_count = 1;
    header.payload_size = request.payload.size();

    std::vector<std::byte> payload(request.payload.begin(), request.payload.end());
    if (implementation_->encryption_enabled) {
        auto nonce = crypto_sodium::create_payload_nonce();
        if (!nonce) {
            return base::Result<void>::failure(nonce.error());
        }
        header.payload_nonce = nonce.value();
        const auto body_size = archive::kBlockEntrySize + payload.size();
        auto aad = make_file_chunk_aad(implementation_->part_header, body_size, header, {&entry, 1});
        if (!aad) {
            return base::Result<void>::failure(aad.error());
        }
        auto protected_payload =
            implementation_->payload_cipher->protect(payload, aad.value(), nonce.value());
        if (!protected_payload) {
            return base::Result<void>::failure(protected_payload.error());
        }
        payload = std::move(protected_payload.value().ciphertext);
        header.payload_authentication_tag = protected_payload.value().tag;
        header.payload_size = payload.size();
    } else {
        auto nonce = crypto_sodium::create_payload_nonce();
        if (!nonce) {
            return base::Result<void>::failure(nonce.error());
        }
        header.payload_nonce = nonce.value();
    }

    const auto body_size = archive::kBlockEntrySize + payload.size();
    auto prefix =
        archive::encode_archive_record_prefix(archive::make_file_stream_chunk_record_prefix(body_size));
    auto encoded_header = archive::encode_file_stream_chunk_header(header);
    auto encoded_entry = archive::encode_block_entry(entry);
    if (!prefix || !encoded_header || !encoded_entry) {
        return base::Result<void>::failure(!prefix           ? prefix.error()
                                           : !encoded_header ? encoded_header.error()
                                                             : encoded_entry.error());
    }
    for (const auto bytes : {std::span<const std::byte>(prefix.value()),
                             std::span<const std::byte>(encoded_header.value()),
                             std::span<const std::byte>(encoded_entry.value()),
                             std::span<const std::byte>(payload)}) {
        auto written = write_bytes(implementation_->output, bytes);
        if (!written) {
            return written;
        }
    }
    ++implementation_->next_chunk_index;
    ++implementation_->total_block_entries;
    implementation_->total_payload_size += payload.size();
    implementation_->logical_bytes += request.logical_size;
    return base::Result<void>::success();
}

base::Result<void>
PersonalFileArchiveSession::finalize(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || implementation_->finalized) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive cannot be finalized"));
    }
    if (implementation_->entry_count == 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive requires at least one entry"));
    }
    implementation_->spool.flush();
    implementation_->spool.close();
    std::ifstream spool_input(implementation_->spool_path, std::ios::binary);
    if (!spool_input) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to open index spool"));
    }
    auto entries = load_spool_entries(spool_input, implementation_->entry_count);
    if (!entries) {
        return base::Result<void>::failure(entries.error());
    }
    auto leaf_groups = pack_leaf_pages(entries.value());
    if (!leaf_groups) {
        return base::Result<void>::failure(leaf_groups.error());
    }
    if (leaf_groups.value().size() > index::kMaximumInternalKeysPerPage + 1U) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_depth_limit"));
    }

    std::vector<std::uint64_t> leaf_page_ids;
    std::vector<index::IndexKey> leaf_first_keys;
    std::array<std::byte, 32> first_leaf_digest{};
    std::uint64_t first_leaf_offset = 0;
    leaf_page_ids.reserve(leaf_groups.value().size());
    leaf_first_keys.reserve(leaf_groups.value().size());
    for (std::size_t leaf_index = 0; leaf_index < leaf_groups.value().size(); ++leaf_index) {
        const auto& group = leaf_groups.value()[leaf_index];
        auto first_key = index::make_index_key(group.front());
        if (!first_key) {
            return base::Result<void>::failure(first_key.error());
        }
        const auto page_id = implementation_->next_page_id++;
        auto prepared = prepare_leaf_index_page(
            group, page_id, implementation_->encryption_enabled,
            implementation_->index_cipher.get(), implementation_->part_header);
        if (!prepared) {
            return base::Result<void>::failure(prepared.error());
        }
        const auto page_offset = implementation_->output.tellp();
        if (page_offset < 0) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kIoFailure, "failed to locate file index page offset"));
        }
        auto written =
            implementation_->write_index_page(prepared.value().header, prepared.value().ciphertext);
        if (!written) {
            return written;
        }
        if (leaf_index == 0) {
            first_leaf_offset = static_cast<std::uint64_t>(page_offset);
            first_leaf_digest = prepared.value().header.content_digest;
        }
        leaf_page_ids.push_back(page_id);
        leaf_first_keys.push_back(std::move(first_key).value());
    }

    std::uint64_t root_page_id = leaf_page_ids.front();
    std::uint64_t root_offset = first_leaf_offset;
    std::array<std::byte, 32> root_content_digest = first_leaf_digest;
    std::uint64_t page_count = leaf_page_ids.size();
    if (leaf_page_ids.size() > 1) {
        index::InternalPageBody internal;
        internal.children = leaf_page_ids;
        internal.keys.reserve(leaf_first_keys.size() - 1);
        for (std::size_t key_index = 1; key_index < leaf_first_keys.size(); ++key_index) {
            internal.keys.push_back(leaf_first_keys[key_index]);
        }
        root_page_id = implementation_->next_page_id++;
        auto prepared = prepare_internal_index_page(
            internal, root_page_id, implementation_->encryption_enabled,
            implementation_->index_cipher.get(), implementation_->part_header);
        if (!prepared) {
            return base::Result<void>::failure(prepared.error());
        }
        const auto page_offset = implementation_->output.tellp();
        if (page_offset < 0) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kIoFailure, "failed to locate file index root offset"));
        }
        auto written =
            implementation_->write_index_page(prepared.value().header, prepared.value().ciphertext);
        if (!written) {
            return written;
        }
        root_offset = static_cast<std::uint64_t>(page_offset);
        root_content_digest = prepared.value().header.content_digest;
        ++page_count;
    }

    auto preimage = index::make_index_root_digest_preimage(
        root_page_id, page_count, implementation_->entry_count, implementation_->stream_count,
        root_content_digest);
    auto index_root_digest = crypto_sodium::sha256(preimage);
    if (!index_root_digest) {
        return base::Result<void>::failure(index_root_digest.error());
    }
    const auto footer_offset = implementation_->output.tellp();
    if (footer_offset < 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to locate file archive footer"));
    }
    archive::BackupFooter footer;
    footer.file_stream_chunk_count = implementation_->next_chunk_index;
    footer.index_page_count = page_count;
    footer.total_block_entry_count = implementation_->total_block_entries;
    footer.total_payload_size = implementation_->total_payload_size;
    footer.logical_bytes = implementation_->logical_bytes;
    footer.entry_count = implementation_->entry_count;
    footer.stream_count = implementation_->stream_count;
    footer.index_root_offset = root_offset;
    footer.index_root_page_id = root_page_id;
    footer.index_root_digest = index_root_digest.value();
    footer.part_file_size =
        static_cast<std::uint64_t>(footer_offset) + archive::kBackupFooterSize;
    footer.stored_bytes = footer.part_file_size;
    footer.file_uuid = implementation_->file_uuid;
    auto encoded_footer = archive::encode_backup_footer(footer);
    if (!encoded_footer) {
        return base::Result<void>::failure(encoded_footer.error());
    }
    auto written = write_bytes(implementation_->output, encoded_footer.value());
    implementation_->output.flush();
    if (!written || !implementation_->output) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to write file archive footer"));
    }
    implementation_->finalized = true;
    return base::Result<void>::success();
}

base::Result<void>
PersonalFileArchiveSession::commit(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || !implementation_->finalized) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive is not finalized"));
    }
    implementation_->output.close();
    std::error_code filesystem_error;
    std::filesystem::rename(implementation_->partial, implementation_->destination,
                            filesystem_error);
    if (filesystem_error) {
        abort();
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to publish file archive"));
    }
    std::filesystem::remove(implementation_->spool_path, filesystem_error);
    implementation_->complete = true;
    implementation_->password.clear();
    return base::Result<void>::success();
}

void PersonalFileArchiveSession::abort() noexcept {
    if (!implementation_ || implementation_->complete) {
        return;
    }
    try {
        implementation_->output.close();
        implementation_->spool.close();
    } catch (...) {
        static_cast<void>(0);
    }
    std::error_code ignored;
    std::filesystem::remove(implementation_->partial, ignored);
    std::filesystem::remove(implementation_->spool_path, ignored);
}

} // namespace aegra::adapters::personal_archive
