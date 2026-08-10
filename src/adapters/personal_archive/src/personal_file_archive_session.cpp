#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_file_archive_secondary_index.h"
#include "win32_output_file.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/crypto_sodium/metadata_crypto.h"
#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/adapters/crypto_sodium/secure_string.h"
#include "aegra/base/error.h"
#include "aegra/format/file_index.h"
#include "aegra/format/manifest_codec.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <set>
#include <span>
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

[[nodiscard]] bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] char* as_mutable_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<void> write_bytes(detail::Win32OutputFile& output,
                                             const std::span<const std::byte> bytes) {
    return output.write(bytes);
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
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument,
                  "unencrypted file archive must not supply a password"));
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
    if (is_zero_uuid(request.file_uuid) || is_zero_uuid(request.backup_set_uuid)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive identity UUIDs are required"));
    }
    const auto backup_type = request.manifest.backup_job.backup_type;
    if (backup_type == format::BackupType::kFull) {
        if (!is_zero_uuid(request.parent_uuid)) {
            return base::Result<void>::failure(error(base::ErrorCode::kInvalidArgument,
                                                     "full file archive must not set parent_uuid"));
        }
    } else if (backup_type == format::BackupType::kIncremental) {
        if (is_zero_uuid(request.parent_uuid) || request.parent_uuid == request.file_uuid) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kInvalidArgument,
                      "incremental file archive requires a distinct non-zero parent_uuid"));
        }
    } else {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive backup type is unsupported"));
    }
    return format::validate_manifest(request.manifest);
}

struct SpooledIndexValidation final {
    std::set<std::uint64_t> entry_ids;
    std::set<std::uint32_t> stream_indices;
};

/// Finalize-time Index rules for Full vs Incremental (syntax only; parent payload is FI5).
[[nodiscard]] base::Result<void> validate_spooled_entry(const contracts::FileEntryDesc& entry,
                                                        const format::BackupType backup_type,
                                                        SpooledIndexValidation& state) {
    auto valid = contracts::validate_file_entry_desc(entry);
    if (!valid) {
        return valid;
    }
    if (!state.entry_ids.insert(entry.entry_id).second) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive entry_id is not unique"));
    }
    for (const auto& stream : entry.streams) {
        if (!state.stream_indices.insert(stream.stream_index).second) {
            return base::Result<void>::failure(error(base::ErrorCode::kInvalidArgument,
                                                     "file archive stream_index is not unique"));
        }
        if (stream.content_storage == contracts::FileContentStorage::kParent) {
            if (backup_type != format::BackupType::kIncremental) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kInvalidArgument,
                          "full file archive cannot contain parent streams"));
            }
        }
    }
    return base::Result<void>::success();
}

/// Compact spool locator: sort by IndexKey without retaining full FileEntryDesc (M5 / L31).
struct SpooledEntryRef final {
    std::uint64_t body_offset{0};
    std::uint32_t body_size{0};
    index::IndexKey key;
};

[[nodiscard]] base::Result<std::vector<std::byte>>
make_file_chunk_aad(const archive::EncodedBackupHeader& part_header, const std::uint64_t body_size,
                    const archive::FileStreamChunkHeader& header,
                    const std::span<const archive::BlockEntry> entries) {
    auto prefix = archive::encode_archive_record_prefix(
        archive::make_file_stream_chunk_record_prefix(body_size));
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
    auto prefix = archive::encode_archive_record_prefix(
        archive::make_file_index_page_record_prefix(body_size));
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

[[nodiscard]] base::Result<std::uint32_t> read_spool_size_prefix(std::ifstream& spool_input) {
    std::array<std::byte, 4> size_bytes{};
    spool_input.read(as_mutable_chars(size_bytes.data()), 4);
    if (!spool_input) {
        return base::Result<std::uint32_t>::failure(
            error(base::ErrorCode::kCorruptData, "index spool is truncated"));
    }
    std::uint32_t size = 0;
    for (std::size_t i = 0; i < 4; ++i) {
        size |= static_cast<std::uint32_t>(std::to_integer<std::uint8_t>(size_bytes[i]))
                << (i * 8U);
    }
    return base::Result<std::uint32_t>::success(size);
}

[[nodiscard]] base::Result<contracts::FileEntryDesc>
read_spool_entry_at(std::ifstream& spool_input, const SpooledEntryRef& ref) {
    spool_input.clear();
    spool_input.seekg(static_cast<std::streamoff>(ref.body_offset), std::ios::beg);
    std::vector<std::byte> encoded(ref.body_size);
    if (ref.body_size != 0) {
        spool_input.read(as_mutable_chars(encoded.data()),
                         static_cast<std::streamsize>(ref.body_size));
    }
    if (!spool_input) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kCorruptData, "index spool entry is truncated"));
    }
    return index::decode_leaf_entry_cbor(encoded);
}

/// First pass: validate each entry, keep only sort key + spool offsets (no full entry table).
[[nodiscard]] base::Result<std::vector<SpooledEntryRef>>
scan_spool_entry_refs(std::ifstream& spool_input, const std::uint64_t entry_count,
                      const format::BackupType backup_type) {
    std::vector<SpooledEntryRef> refs;
    refs.reserve(static_cast<std::size_t>(entry_count));
    SpooledIndexValidation validation;
    for (std::uint64_t index = 0; index < entry_count; ++index) {
        auto size = read_spool_size_prefix(spool_input);
        if (!size) {
            return base::Result<std::vector<SpooledEntryRef>>::failure(size.error());
        }
        const auto body_offset = static_cast<std::uint64_t>(spool_input.tellg());
        std::vector<std::byte> encoded(size.value());
        if (size.value() != 0) {
            spool_input.read(as_mutable_chars(encoded.data()),
                             static_cast<std::streamsize>(size.value()));
        }
        if (!spool_input) {
            return base::Result<std::vector<SpooledEntryRef>>::failure(
                error(base::ErrorCode::kCorruptData, "index spool entry is truncated"));
        }
        auto entry = index::decode_leaf_entry_cbor(encoded);
        if (!entry) {
            return base::Result<std::vector<SpooledEntryRef>>::failure(entry.error());
        }
        auto valid = validate_spooled_entry(entry.value(), backup_type, validation);
        if (!valid) {
            return base::Result<std::vector<SpooledEntryRef>>::failure(valid.error());
        }
        auto key = index::make_index_key(entry.value());
        if (!key) {
            return base::Result<std::vector<SpooledEntryRef>>::failure(key.error());
        }
        SpooledEntryRef ref;
        ref.body_offset = body_offset;
        ref.body_size = size.value();
        ref.key = std::move(key).value();
        refs.push_back(std::move(ref));
    }
    std::ranges::sort(refs, [](const SpooledEntryRef& left, const SpooledEntryRef& right) {
        return index::compare_index_keys(left.key, right.key) < 0;
    });
    return base::Result<std::vector<SpooledEntryRef>>::success(std::move(refs));
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
        return base::Result<PreparedIndexPage>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
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

[[nodiscard]] base::Result<PreparedIndexPage> prepare_internal_index_page(
    const index::InternalPageBody& body, const std::uint64_t page_id, const bool encryption_enabled,
    crypto_sodium::PayloadCipher* payload_cipher, const archive::EncodedBackupHeader& part_header) {
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

/// One written Index page used as a child when building the next internal level.
struct BuiltIndexPage final {
    std::uint64_t page_id{0};
    index::IndexKey first_key;
    std::array<std::byte, 32> content_digest{};
    std::uint64_t file_offset{0};
};

struct NamespaceWriteResult final {
    std::vector<BuiltIndexPage> leaves;
    std::vector<index::EntryIdIndexRecord> entry_records;
    std::vector<index::StreamIndexRecord> stream_records;
};

[[nodiscard]] base::Result<void> emit_index_page(detail::Win32OutputFile& output,
                                                 const archive::FileIndexPageHeader& header,
                                                 std::span<const std::byte> ciphertext);

[[nodiscard]] base::Result<BuiltIndexPage>
write_one_leaf_page(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                    const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                    const archive::EncodedBackupHeader& part_header,
                    const std::vector<contracts::FileEntryDesc>& group) {
    auto first_key = index::make_index_key(group.front());
    if (!first_key) {
        return base::Result<BuiltIndexPage>::failure(first_key.error());
    }
    const auto page_id = next_page_id++;
    auto prepared =
        prepare_leaf_index_page(group, page_id, encryption_enabled, index_cipher, part_header);
    if (!prepared) {
        return base::Result<BuiltIndexPage>::failure(prepared.error());
    }
    const auto page_offset = output.position();
    auto written = emit_index_page(output, prepared.value().header, prepared.value().ciphertext);
    if (!written) {
        return base::Result<BuiltIndexPage>::failure(written.error());
    }
    BuiltIndexPage built;
    built.page_id = page_id;
    built.first_key = std::move(first_key).value();
    built.content_digest = prepared.value().header.content_digest;
    built.file_offset = page_offset;
    return base::Result<BuiltIndexPage>::success(std::move(built));
}

void append_secondary_records_for_leaf(const std::vector<contracts::FileEntryDesc>& group,
                                       const BuiltIndexPage& leaf, NamespaceWriteResult& out) {
    for (std::size_t slot = 0; slot < group.size(); ++slot) {
        const auto& entry = group[slot];
        index::EntryIdIndexRecord entry_rec;
        entry_rec.entry_id = entry.entry_id;
        entry_rec.page_id = leaf.page_id;
        entry_rec.page_offset = leaf.file_offset;
        entry_rec.slot = static_cast<std::uint32_t>(slot);
        entry_rec.parent_entry_id = entry.parent_entry_id;
        entry_rec.kind = static_cast<std::uint8_t>(entry.kind);
        out.entry_records.push_back(entry_rec);
        for (std::size_t stream_i = 0; stream_i < entry.streams.size(); ++stream_i) {
            index::StreamIndexRecord stream_rec;
            stream_rec.stream_index = entry.streams[stream_i].stream_index;
            stream_rec.entry_id = entry.entry_id;
            stream_rec.stream_slot = static_cast<std::uint32_t>(stream_i);
            out.stream_records.push_back(stream_rec);
        }
    }
}

/// Stream leaf packing: at most one leaf of FileEntryDesc resident (M5).
[[nodiscard]] base::Result<NamespaceWriteResult> write_leaves_from_sorted_spool(
    detail::Win32OutputFile& output, std::ifstream& spool_input, std::uint64_t& next_page_id,
    const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
    const archive::EncodedBackupHeader& part_header, const std::vector<SpooledEntryRef>& refs) {
    if (refs.empty()) {
        return base::Result<NamespaceWriteResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive requires at least one entry"));
    }
    NamespaceWriteResult result;
    result.entry_records.reserve(refs.size());
    std::vector<contracts::FileEntryDesc> current;
    current.reserve(
        (std::min)(refs.size(), static_cast<std::size_t>(index::kMaximumLeafEntriesPerPage)));
    for (const auto& ref : refs) {
        auto entry = read_spool_entry_at(spool_input, ref);
        if (!entry) {
            return base::Result<NamespaceWriteResult>::failure(entry.error());
        }
        auto trial = current;
        trial.push_back(std::move(entry).value());
        auto fits = leaf_page_fits(trial);
        if (!fits) {
            return base::Result<NamespaceWriteResult>::failure(fits.error());
        }
        if (fits.value()) {
            current = std::move(trial);
            continue;
        }
        if (current.empty()) {
            return base::Result<NamespaceWriteResult>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
        auto written = write_one_leaf_page(output, next_page_id, encryption_enabled, index_cipher,
                                           part_header, current);
        if (!written) {
            return base::Result<NamespaceWriteResult>::failure(written.error());
        }
        append_secondary_records_for_leaf(current, written.value(), result);
        result.leaves.push_back(std::move(written).value());
        current.clear();
        current.push_back(std::move(trial.back()));
        auto single = leaf_page_fits(current);
        if (!single) {
            return base::Result<NamespaceWriteResult>::failure(single.error());
        }
        if (!single.value()) {
            return base::Result<NamespaceWriteResult>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
    }
    if (!current.empty()) {
        auto written = write_one_leaf_page(output, next_page_id, encryption_enabled, index_cipher,
                                           part_header, current);
        if (!written) {
            return base::Result<NamespaceWriteResult>::failure(written.error());
        }
        append_secondary_records_for_leaf(current, written.value(), result);
        result.leaves.push_back(std::move(written).value());
    }
    return base::Result<NamespaceWriteResult>::success(std::move(result));
}

struct IndexTreeRoot final {
    std::uint64_t root_page_id{0};
    std::uint64_t root_offset{0};
    std::array<std::byte, 32> root_content_digest{};
    std::uint64_t page_count{0};
};

[[nodiscard]] base::Result<void> emit_index_page(detail::Win32OutputFile& output,
                                                 const archive::FileIndexPageHeader& header,
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

[[nodiscard]] base::Result<index::InternalPageBody>
make_internal_body(const std::span<const BuiltIndexPage> children) {
    if (children.size() < 2 ||
        children.size() > static_cast<std::size_t>(index::kMaximumInternalKeysPerPage) + 1U) {
        return base::Result<index::InternalPageBody>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
    }
    index::InternalPageBody body;
    body.children.reserve(children.size());
    body.keys.reserve(children.size() - 1);
    for (const auto& child : children) {
        if (child.page_id == 0 || child.file_offset == 0) {
            return base::Result<index::InternalPageBody>::failure(
                error(base::ErrorCode::kInternal, "index child locator is incomplete"));
        }
        body.children.push_back(index::ChildPageLocator{child.page_id, child.file_offset});
    }
    for (std::size_t i = 1; i < children.size(); ++i) {
        body.keys.push_back(children[i].first_key);
    }
    return base::Result<index::InternalPageBody>::success(std::move(body));
}

[[nodiscard]] base::Result<bool>
internal_page_fits(const std::span<const BuiltIndexPage> children) {
    auto body = make_internal_body(children);
    if (!body) {
        if (body.error().code == base::ErrorCode::kInvalidArgument) {
            return base::Result<bool>::success(false);
        }
        return base::Result<bool>::failure(body.error());
    }
    auto plain = index::encode_internal_page_cbor(body.value());
    if (!plain) {
        return base::Result<bool>::failure(plain.error());
    }
    return base::Result<bool>::success(plain.value().size() <=
                                       archive::kMaximumIndexPagePlainBytes);
}

/// Largest fanout in [2, remaining] that fits plain-size limits; never leaves a single orphan
/// child.
[[nodiscard]] base::Result<std::size_t>
choose_internal_fanout(const std::span<const BuiltIndexPage> level, const std::size_t start) {
    if (start >= level.size()) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInternal, "internal index pack start is invalid"));
    }
    const auto remaining = level.size() - start;
    if (remaining < 2) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
    }
    const auto max_take =
        (std::min)(remaining, static_cast<std::size_t>(index::kMaximumInternalKeysPerPage) + 1U);
    for (auto take = max_take; take >= 2; --take) {
        // Internal pages need ≥2 children; skip sizes that would leave exactly one child behind.
        if (remaining > take && remaining - take == 1) {
            continue;
        }
        auto fits = internal_page_fits(level.subspan(start, take));
        if (!fits) {
            return base::Result<std::size_t>::failure(fits.error());
        }
        if (fits.value()) {
            return base::Result<std::size_t>::success(take);
        }
    }
    return base::Result<std::size_t>::failure(
        error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
}

[[nodiscard]] base::Result<BuiltIndexPage>
write_internal_group(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                     const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const archive::EncodedBackupHeader& part_header,
                     const std::span<const BuiltIndexPage> children) {
    auto body = make_internal_body(children);
    if (!body) {
        return base::Result<BuiltIndexPage>::failure(body.error());
    }
    const auto page_id = next_page_id++;
    auto prepared = prepare_internal_index_page(body.value(), page_id, encryption_enabled,
                                                index_cipher, part_header);
    if (!prepared) {
        return base::Result<BuiltIndexPage>::failure(prepared.error());
    }
    const auto page_offset = output.position();
    auto written = emit_index_page(output, prepared.value().header, prepared.value().ciphertext);
    if (!written) {
        return base::Result<BuiltIndexPage>::failure(written.error());
    }
    BuiltIndexPage built;
    built.page_id = page_id;
    built.first_key = children.front().first_key;
    built.content_digest = prepared.value().header.content_digest;
    built.file_offset = page_offset;
    return base::Result<BuiltIndexPage>::success(std::move(built));
}

[[nodiscard]] base::Result<std::vector<BuiltIndexPage>>
write_internal_level(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                     const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const archive::EncodedBackupHeader& part_header,
                     const std::vector<BuiltIndexPage>& children) {
    if (children.size() < 2) {
        return base::Result<std::vector<BuiltIndexPage>>::failure(
            error(base::ErrorCode::kInternal, "internal index level requires multiple children"));
    }
    std::vector<BuiltIndexPage> parents;
    std::size_t start = 0;
    while (start < children.size()) {
        auto take = choose_internal_fanout(children, start);
        if (!take) {
            return base::Result<std::vector<BuiltIndexPage>>::failure(take.error());
        }
        auto written = write_internal_group(
            output, next_page_id, encryption_enabled, index_cipher, part_header,
            std::span<const BuiltIndexPage>(children.data() + start, take.value()));
        if (!written) {
            return base::Result<std::vector<BuiltIndexPage>>::failure(written.error());
        }
        parents.push_back(std::move(written).value());
        start += take.value();
    }
    if (parents.empty() || parents.size() >= children.size()) {
        return base::Result<std::vector<BuiltIndexPage>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_depth_limit"));
    }
    return base::Result<std::vector<BuiltIndexPage>>::success(std::move(parents));
}

/// Bottom-up B+tree: given written leaves, raise internal levels until a single root (depth ≤ 8).
[[nodiscard]] base::Result<IndexTreeRoot>
raise_index_tree_to_root(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                         const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                         const archive::EncodedBackupHeader& part_header,
                         std::vector<BuiltIndexPage> level) {
    if (level.empty()) {
        return base::Result<IndexTreeRoot>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive requires at least one entry"));
    }
    std::uint64_t page_count = level.size();
    // levels_above_leaves becomes the depth of every leaf (root depth = 0).
    std::uint32_t levels_above_leaves = 0;
    while (level.size() > 1) {
        ++levels_above_leaves;
        if (levels_above_leaves > index::kMaximumIndexDepth) {
            return base::Result<IndexTreeRoot>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_depth_limit"));
        }
        auto parents = write_internal_level(output, next_page_id, encryption_enabled, index_cipher,
                                            part_header, level);
        if (!parents) {
            return base::Result<IndexTreeRoot>::failure(parents.error());
        }
        page_count += parents.value().size();
        level = std::move(parents).value();
    }
    const auto& root = level.front();
    IndexTreeRoot result;
    result.root_page_id = root.page_id;
    result.root_offset = root.file_offset;
    result.root_content_digest = root.content_digest;
    result.page_count = page_count;
    return base::Result<IndexTreeRoot>::success(result);
}

} // namespace

struct PersonalFileArchiveSession::Impl final {
    explicit Impl(const std::string_view archive_password, const bool archive_encryption_enabled)
        : password(archive_password), encryption_enabled(archive_encryption_enabled) {}

    std::filesystem::path destination;
    std::filesystem::path partial;
    std::filesystem::path spool_path;
    detail::Win32OutputFile output;
    detail::Win32OutputFile spool;
    crypto_sodium::SecureString password;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    std::unique_ptr<crypto_sodium::PayloadCipher> index_cipher;
    bool encryption_enabled{true};
    format::BackupType backup_type{format::BackupType::kFull};
    archive::EncodedBackupHeader part_header{};
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> parent_uuid{};
    std::uint32_t block_size{0};
    std::uint64_t next_chunk_index{0};
    std::uint64_t total_block_entries{0};
    std::uint64_t total_payload_size{0};
    /// Local payload logical bytes written this layer (parent streams excluded).
    std::uint64_t logical_bytes{0};
    std::uint64_t entry_count{0};
    std::uint64_t stream_count{0};
    std::uint64_t local_stream_count{0};
    std::uint64_t parent_stream_count{0};
    std::uint64_t next_page_id{1};
    /// Chunk locators for secondary Chunk Index (ADR-0019); filled by write_stream_chunk.
    std::vector<index::ChunkIndexRecord> chunk_records;
    bool finalized{false};
    bool complete{false};
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
            !cbor
                ? cbor.error()
                : error(base::ErrorCode::kInvalidArgument, "file archive metadata exceeds limit"));
    }

    archive::BackupHeader logical_header;
    logical_header.file_uuid = request.file_uuid;
    logical_header.backup_set_uuid = request.backup_set_uuid;
    logical_header.parent_uuid = request.parent_uuid;
    logical_header.block_size = request.block_size;
    logical_header.content_kind = archive::kContentKindFileSet;
    logical_header.capability_flags = archive::kCapabilityHasFileIndex;
    logical_header.default_chunk_size = request.chunk_size;
    // Default method for file stream blocks (opportunistic zstd; RAW when no gain).
    logical_header.compression_method = archive::CompressionMethod::kZstandard;
    if (request.manifest.backup_job.backup_type == format::BackupType::kIncremental) {
        logical_header.flags = archive::kBackupFlagIncremental;
        logical_header.capability_flags |= archive::kCapabilityFileMetadataBaseline;
    } else {
        logical_header.flags = archive::kBackupFlagFull;
    }
    if (request.encryption_enabled) {
        logical_header.flags |= archive::kBackupFlagEncrypted;
        logical_header.encryption_method = archive::PayloadEncryptionMethod::kXChaCha20Poly1305;
    }

    auto implementation = std::make_unique<Impl>(request.password, request.encryption_enabled);
    implementation->destination = request.destination;
    implementation->partial = partial;
    implementation->spool_path = request.index_spool_directory / "index.spool";
    implementation->file_uuid = request.file_uuid;
    implementation->parent_uuid = request.parent_uuid;
    implementation->backup_type = request.manifest.backup_job.backup_type;
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
        logical_header.first_record_offset = logical_header.cbor_offset + logical_header.cbor_size;
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
        auto cipher =
            crypto_sodium::PayloadCipher::create(request.password, metadata.kdf, metadata.salt);
        if (!cipher) {
            return base::Result<std::unique_ptr<PersonalFileArchiveSession>>::failure(
                cipher.error());
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
        logical_header.first_record_offset = logical_header.cbor_offset + logical_header.cbor_size;
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

    auto output_opened = implementation->output.open(partial);
    auto spool_opened = implementation->spool.open(implementation->spool_path);
    if (!output_opened || !spool_opened) {
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
    auto valid = contracts::validate_file_entry_desc(entry);
    if (!valid) {
        return valid;
    }
    std::uint64_t entry_parent_streams = 0;
    std::uint64_t entry_local_streams = 0;
    for (const auto& stream : entry.streams) {
        if (stream.content_storage == contracts::FileContentStorage::kParent) {
            if (implementation_->backup_type != format::BackupType::kIncremental) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kInvalidArgument,
                          "full file archive cannot contain parent streams"));
            }
            ++entry_parent_streams;
        } else {
            ++entry_local_streams;
        }
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
    std::vector<std::byte> spool_record;
    spool_record.reserve(size_bytes.size() + encoded.value().size());
    spool_record.insert(spool_record.end(), size_bytes.begin(), size_bytes.end());
    spool_record.insert(spool_record.end(), encoded.value().begin(), encoded.value().end());
    auto written = write_bytes(implementation_->spool, spool_record);
    if (!written) {
        return written;
    }
    ++implementation_->entry_count;
    implementation_->stream_count += entry.streams.size();
    implementation_->parent_stream_count += entry_parent_streams;
    implementation_->local_stream_count += entry_local_streams;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<std::byte>>
store_file_block_payload(const std::span<const std::byte> logical, std::uint8_t& flags,
                         std::uint32_t& stored_size) {
    auto compressed = compression_zstd::compress(logical);
    if (!compressed) {
        return base::Result<std::vector<std::byte>>::failure(compressed.error());
    }
    if (compressed.value().size() < logical.size() &&
        compressed.value().size() <= (std::numeric_limits<std::uint32_t>::max)()) {
        flags = archive::kBlockFlagCompressed;
        stored_size = static_cast<std::uint32_t>(compressed.value().size());
        return base::Result<std::vector<std::byte>>::success(std::move(compressed).value());
    }
    if (logical.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream chunk exceeds format limit"));
    }
    flags = archive::kBlockFlagRaw;
    stored_size = static_cast<std::uint32_t>(logical.size());
    return base::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(logical.begin(), logical.end()));
}

base::Result<std::uint64_t>
PersonalFileArchiveSession::write_stream_chunk(const ports::FileChunkWriteRequest& request,
                                               const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kCancelled, "backup cancelled"));
    }
    if (!implementation_ || implementation_->complete || implementation_->finalized) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive session is not writable"));
    }
    if (request.chunk_index != implementation_->next_chunk_index || request.stream_index == 0 ||
        request.logical_size == 0 || request.logical_size > implementation_->block_size ||
        request.payload.size() != request.logical_size) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream chunk descriptor is invalid"));
    }
    // Payload is always logical; opportunistic zstd (V7 §8). block_flags from caller ignored.
    (void)request.block_flags;
    std::uint8_t flags = archive::kBlockFlagRaw;
    std::uint32_t stored_size = 0;
    auto stored = store_file_block_payload(request.payload, flags, stored_size);
    if (!stored) {
        return base::Result<std::uint64_t>::failure(stored.error());
    }

    archive::BlockEntry entry;
    entry.logical_block_index = request.logical_block_index;
    entry.data_offset_or_reference = 0;
    entry.stored_size = stored_size;
    entry.logical_size = request.logical_size;
    entry.flags = flags;

    archive::FileStreamChunkHeader header;
    header.chunk_index = request.chunk_index;
    header.source_type = archive::kSourceTypeFileStream;
    header.source_index = request.stream_index;
    header.block_entry_count = 1;
    header.payload_size = stored.value().size();

    std::vector<std::byte> payload = std::move(stored).value();
    if (implementation_->encryption_enabled) {
        auto nonce = crypto_sodium::create_payload_nonce();
        if (!nonce) {
            return base::Result<std::uint64_t>::failure(nonce.error());
        }
        header.payload_nonce = nonce.value();
        const auto body_size = archive::kBlockEntrySize + payload.size();
        auto aad =
            make_file_chunk_aad(implementation_->part_header, body_size, header, {&entry, 1});
        if (!aad) {
            return base::Result<std::uint64_t>::failure(aad.error());
        }
        auto protected_payload =
            implementation_->payload_cipher->protect(payload, aad.value(), nonce.value());
        if (!protected_payload) {
            return base::Result<std::uint64_t>::failure(protected_payload.error());
        }
        payload = std::move(protected_payload.value().ciphertext);
        header.payload_authentication_tag = protected_payload.value().tag;
        header.payload_size = payload.size();
    } else {
        auto nonce = crypto_sodium::create_payload_nonce();
        if (!nonce) {
            return base::Result<std::uint64_t>::failure(nonce.error());
        }
        header.payload_nonce = nonce.value();
    }

    const auto body_size = archive::kBlockEntrySize + payload.size();
    auto prefix = archive::encode_archive_record_prefix(
        archive::make_file_stream_chunk_record_prefix(body_size));
    auto encoded_header = archive::encode_file_stream_chunk_header(header);
    auto encoded_entry = archive::encode_block_entry(entry);
    if (!prefix || !encoded_header || !encoded_entry) {
        return base::Result<std::uint64_t>::failure(!prefix           ? prefix.error()
                                                    : !encoded_header ? encoded_header.error()
                                                                      : encoded_entry.error());
    }
    const auto record_offset = implementation_->output.position();
    std::vector<std::byte> record_header;
    record_header.reserve(prefix.value().size() + encoded_header.value().size() +
                          encoded_entry.value().size());
    record_header.insert(record_header.end(), prefix.value().begin(), prefix.value().end());
    record_header.insert(record_header.end(), encoded_header.value().begin(),
                         encoded_header.value().end());
    record_header.insert(record_header.end(), encoded_entry.value().begin(),
                         encoded_entry.value().end());
    auto header_written = write_bytes(implementation_->output, record_header);
    if (!header_written) {
        return base::Result<std::uint64_t>::failure(header_written.error());
    }
    auto payload_written = write_bytes(implementation_->output, payload);
    if (!payload_written) {
        return base::Result<std::uint64_t>::failure(payload_written.error());
    }
    index::ChunkIndexRecord chunk_rec;
    chunk_rec.chunk_index = header.chunk_index;
    chunk_rec.record_offset = record_offset;
    chunk_rec.payload_offset = record_offset +
                               archive::kArchiveRecordPrefixSize +
                               archive::kFileStreamChunkHeaderSize + archive::kBlockEntrySize;
    chunk_rec.payload_size = payload.size();
    chunk_rec.block_entry_count = 1;
    implementation_->chunk_records.push_back(chunk_rec);
    ++implementation_->next_chunk_index;
    ++implementation_->total_block_entries;
    implementation_->total_payload_size += payload.size();
    implementation_->logical_bytes += request.logical_size;
    return base::Result<std::uint64_t>::success(stored_size);
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
    auto spool_flushed = implementation_->spool.flush();
    implementation_->spool.close();
    if (!spool_flushed) {
        return base::Result<void>::failure(spool_flushed.error());
    }
    std::ifstream spool_input(implementation_->spool_path, std::ios::binary);
    if (!spool_input) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to open index spool"));
    }
    // M5: compact sort keys + stream leaf write (no full FileEntryDesc table).
    auto refs = scan_spool_entry_refs(spool_input, implementation_->entry_count,
                                      implementation_->backup_type);
    if (!refs) {
        return base::Result<void>::failure(refs.error());
    }
    if (refs.value().size() != implementation_->entry_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "index spool entry count mismatch"));
    }
    auto namespace_write = write_leaves_from_sorted_spool(
        implementation_->output, spool_input, implementation_->next_page_id,
        implementation_->encryption_enabled, implementation_->index_cipher.get(),
        implementation_->part_header, refs.value());
    if (!namespace_write) {
        return base::Result<void>::failure(namespace_write.error());
    }
    auto namespace_pages = std::move(namespace_write).value();
    // Namespace B+tree (V7 §5.6 / L14).
    auto tree = raise_index_tree_to_root(
        implementation_->output, implementation_->next_page_id, implementation_->encryption_enabled,
        implementation_->index_cipher.get(), implementation_->part_header,
        std::move(namespace_pages.leaves));
    if (!tree) {
        return base::Result<void>::failure(tree.error());
    }
    const auto root_page_id = tree.value().root_page_id;
    const auto root_offset = tree.value().root_offset;
    const auto root_content_digest = tree.value().root_content_digest;
    std::uint64_t page_count = tree.value().page_count;

    // page_count in digest preimage is 0: Footer only stores total index_page_count (all trees).
    auto preimage =
        index::make_index_root_digest_preimage(root_page_id, 0, implementation_->entry_count,
                                               implementation_->stream_count, root_content_digest);
    auto index_root_digest = crypto_sodium::sha256(preimage);
    if (!index_root_digest) {
        return base::Result<void>::failure(index_root_digest.error());
    }

    // ADR-0019 secondary indexes: Entry ID → Stream → Chunk.
    auto entry_tree = secondary_index::write_entry_id_index(
        implementation_->output, implementation_->next_page_id, implementation_->encryption_enabled,
        implementation_->index_cipher.get(), implementation_->part_header,
        std::move(namespace_pages.entry_records));
    if (!entry_tree) {
        return base::Result<void>::failure(entry_tree.error());
    }
    page_count += entry_tree.value().page_count;

    archive::IndexRootLocator stream_root{};
    if (!namespace_pages.stream_records.empty()) {
        auto stream_tree = secondary_index::write_stream_index(
            implementation_->output, implementation_->next_page_id,
            implementation_->encryption_enabled, implementation_->index_cipher.get(),
            implementation_->part_header, std::move(namespace_pages.stream_records));
        if (!stream_tree) {
            return base::Result<void>::failure(stream_tree.error());
        }
        stream_root = stream_tree.value().locator;
        page_count += stream_tree.value().page_count;
    }

    archive::IndexRootLocator chunk_root{};
    if (!implementation_->chunk_records.empty()) {
        auto chunk_tree = secondary_index::write_chunk_index(
            implementation_->output, implementation_->next_page_id,
            implementation_->encryption_enabled, implementation_->index_cipher.get(),
            implementation_->part_header, std::move(implementation_->chunk_records));
        if (!chunk_tree) {
            return base::Result<void>::failure(chunk_tree.error());
        }
        chunk_root = chunk_tree.value().locator;
        page_count += chunk_tree.value().page_count;
    }

    const auto footer_offset = implementation_->output.position();
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
    footer.entry_id_root = entry_tree.value().locator;
    footer.stream_root = stream_root;
    footer.chunk_root = chunk_root;
    footer.part_file_size = footer_offset + archive::kBackupFooterSize;
    footer.stored_bytes = footer.part_file_size;
    footer.file_uuid = implementation_->file_uuid;
    auto encoded_footer = archive::encode_backup_footer(footer);
    if (!encoded_footer) {
        return base::Result<void>::failure(encoded_footer.error());
    }
    auto written = write_bytes(implementation_->output, encoded_footer.value());
    auto flushed = implementation_->output.flush();
    if (!written || !flushed) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "failed to write file archive footer"));
    }
    implementation_->finalized = true;
    return base::Result<void>::success();
}

base::Result<void> PersonalFileArchiveSession::commit(const base::CancellationToken cancellation) {
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
    implementation_->output.close();
    implementation_->spool.close();
    std::error_code ignored;
    std::filesystem::remove(implementation_->partial, ignored);
    std::filesystem::remove(implementation_->spool_path, ignored);
}

} // namespace aegra::adapters::personal_archive
