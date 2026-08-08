#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_preamble.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/error.h"
#include "aegra/format/file_index.h"
#include "aegra/format/personal_archive.h"

#include "aegra/contracts/file_set.h"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;
namespace index = format::file_index;

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] char* as_mutable_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) stream byte-buffer boundary.
    return reinterpret_cast<char*>(value);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact(std::ifstream& input, const std::uint64_t offset, const std::size_t size) {
    if (size > static_cast<std::size_t>((std::numeric_limits<std::streamsize>::max)())) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "file archive read exceeds stream limit"));
    }
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    std::vector<std::byte> result(size);
    if (size != 0) {
        input.read(as_mutable_chars(result.data()), static_cast<std::streamsize>(size));
    }
    if (!input || input.gcount() != static_cast<std::streamsize>(size)) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kIoFailure, "file archive is truncated"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(result));
}

[[nodiscard]] base::Result<std::uint64_t> read_stream_size(std::ifstream& input) {
    input.seekg(0, std::ios::end);
    const auto position = input.tellg();
    if (position < 0) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kIoFailure, "failed to determine file archive size"));
    }
    input.seekg(0, std::ios::beg);
    return base::Result<std::uint64_t>::success(
        static_cast<std::uint64_t>(static_cast<std::streamoff>(position)));
}

[[nodiscard]] std::string digest_to_hex(const std::array<std::byte, 32>& digest) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string result;
    result.resize(64);
    for (std::size_t i = 0; i < digest.size(); ++i) {
        const auto value = std::to_integer<std::uint8_t>(digest[i]);
        result[i * 2] = kHex[value >> 4U];
        result[i * 2 + 1] = kHex[value & 0x0FU];
    }
    return result;
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

struct StreamChunkLocator final {
    archive::FileStreamChunkHeader header;
    std::vector<archive::BlockEntry> entries;
    std::uint64_t payload_offset{0};
};

struct OpenedFileArchive final {
    detail::ParsedPreamble preamble;
    archive::EncodedBackupHeader part_header{};
    archive::BackupFooter footer;
    std::vector<contracts::FileEntryDesc> entries;
    std::unordered_map<std::uint64_t, std::size_t> entry_by_id;
    std::unordered_map<std::uint64_t, StreamChunkLocator> chunks_by_index;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    std::unique_ptr<crypto_sodium::PayloadCipher> index_cipher;
    std::filesystem::path path;
    bool encryption_enabled{false};
};

[[nodiscard]] base::Result<archive::BackupFooter>
read_required_footer(std::ifstream& input, const std::uint64_t file_size) {
    if (file_size < archive::kBackupFooterSize) {
        return base::Result<archive::BackupFooter>::failure(
            error(base::ErrorCode::kCorruptData, "file archive is missing footer"));
    }
    auto footer_bytes =
        read_exact(input, file_size - archive::kBackupFooterSize, archive::kBackupFooterSize);
    if (!footer_bytes) {
        return base::Result<archive::BackupFooter>::failure(footer_bytes.error());
    }
    auto footer = archive::decode_backup_footer(footer_bytes.value());
    if (!footer) {
        return base::Result<archive::BackupFooter>::failure(footer.error());
    }
    if (footer.value().part_file_size != file_size) {
        return base::Result<archive::BackupFooter>::failure(
            error(base::ErrorCode::kCorruptData, "file archive footer size mismatch"));
    }
    return base::Result<archive::BackupFooter>::success(std::move(footer).value());
}

struct DecodedIndexPage final {
    archive::FileIndexPageHeader header;
    std::vector<std::byte> plaintext;
    std::uint64_t page_offset{0};
};

[[nodiscard]] base::Result<DecodedIndexPage>
read_index_page_at(std::ifstream& input, const OpenedFileArchive& archive_state,
                   const std::uint64_t page_offset) {
    auto prefix_bytes = read_exact(input, page_offset, archive::kArchiveRecordPrefixSize);
    if (!prefix_bytes) {
        return base::Result<DecodedIndexPage>::failure(prefix_bytes.error());
    }
    auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
    if (!prefix || prefix.value().record_kind != archive::kRecordKindFileIndexPage) {
        return base::Result<DecodedIndexPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page record is invalid"));
    }
    auto header_bytes = read_exact(input, page_offset + archive::kArchiveRecordPrefixSize,
                                   archive::kFileIndexPageHeaderSize);
    if (!header_bytes) {
        return base::Result<DecodedIndexPage>::failure(header_bytes.error());
    }
    auto page_header = archive::decode_file_index_page_header(header_bytes.value());
    if (!page_header) {
        return base::Result<DecodedIndexPage>::failure(page_header.error());
    }
    if (page_header.value().encoded_size > archive::kMaximumIndexPagePlainBytes ||
        page_header.value().plain_size > archive::kMaximumIndexPagePlainBytes ||
        prefix.value().body_size != page_header.value().encoded_size) {
        return base::Result<DecodedIndexPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page header is invalid"));
    }
    const auto body_offset = page_offset + archive::kFileIndexPageRecordHeaderSize;
    auto body = read_exact(input, body_offset, page_header.value().encoded_size);
    if (!body) {
        return base::Result<DecodedIndexPage>::failure(body.error());
    }
    std::vector<std::byte> plaintext = std::move(body).value();
    if (archive_state.encryption_enabled) {
        auto aad =
            make_index_page_aad(archive_state.part_header, plaintext.size(), page_header.value());
        if (!aad) {
            return base::Result<DecodedIndexPage>::failure(aad.error());
        }
        auto plain = archive_state.index_cipher->unprotect(
            plaintext, aad.value(), page_header.value().nonce,
            page_header.value().authentication_tag);
        if (!plain) {
            return base::Result<DecodedIndexPage>::failure(plain.error());
        }
        plaintext = std::move(plain).value();
    }
    if (plaintext.size() != page_header.value().plain_size) {
        return base::Result<DecodedIndexPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page plain size mismatch"));
    }
    auto digest = crypto_sodium::sha256(plaintext);
    if (!digest || digest.value() != page_header.value().content_digest) {
        return base::Result<DecodedIndexPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page content digest mismatch"));
    }
    DecodedIndexPage decoded;
    decoded.header = std::move(page_header).value();
    decoded.plaintext = std::move(plaintext);
    decoded.page_offset = page_offset;
    return base::Result<DecodedIndexPage>::success(std::move(decoded));
}

// Sequential scan: file_set writes all stream chunks then index pages contiguously.
[[nodiscard]] base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>
map_index_page_offsets(std::ifstream& input, const OpenedFileArchive& archive_state) {
    std::unordered_map<std::uint64_t, std::uint64_t> offsets;
    auto offset = archive_state.footer.index_root_offset;
    // Index pages may start before the root when leaves are written first.
    // Scan from the first record after stream chunks: footer.index_root_offset for single-leaf
    // is the only page; for multi-leaf the root is last, so scan from end of stream chunks.
    offset = archive_state.preamble.header.first_record_offset;
    const auto end_offset = archive_state.footer.part_file_size - archive::kBackupFooterSize;
    while (offset < end_offset) {
        auto prefix_bytes = read_exact(input, offset, archive::kArchiveRecordPrefixSize);
        if (!prefix_bytes) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                prefix_bytes.error());
        }
        auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
        if (!prefix) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                prefix.error());
        }
        if (prefix.value().record_kind == archive::kRecordKindFileStreamChunk) {
            offset += archive::kArchiveRecordPrefixSize + archive::kFileStreamChunkHeaderSize +
                      prefix.value().body_size;
            continue;
        }
        if (prefix.value().record_kind != archive::kRecordKindFileIndexPage) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                error(base::ErrorCode::kCorruptData, "unexpected record while mapping index pages"));
        }
        auto header_bytes = read_exact(input, offset + archive::kArchiveRecordPrefixSize,
                                       archive::kFileIndexPageHeaderSize);
        if (!header_bytes) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                header_bytes.error());
        }
        auto page_header = archive::decode_file_index_page_header(header_bytes.value());
        if (!page_header) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                page_header.error());
        }
        if (!offsets.emplace(page_header.value().page_id, offset).second) {
            return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
                error(base::ErrorCode::kCorruptData, "duplicate file index page id"));
        }
        offset += archive::kFileIndexPageRecordHeaderSize + page_header.value().encoded_size;
    }
    if (offsets.size() != archive_state.footer.index_page_count) {
        return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::failure(
            error(base::ErrorCode::kCorruptData, "file index page count mismatch"));
    }
    return base::Result<std::unordered_map<std::uint64_t, std::uint64_t>>::success(
        std::move(offsets));
}

[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
collect_leaf_entries(std::ifstream& input, const OpenedFileArchive& archive_state,
                     const std::unordered_map<std::uint64_t, std::uint64_t>& page_offsets,
                     const std::uint64_t page_id, const std::uint32_t depth) {
    if (depth > index::kMaximumIndexDepth) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file_backup.index_depth_limit"));
    }
    const auto found = page_offsets.find(page_id);
    if (found == page_offsets.end()) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index page id is missing"));
    }
    auto page = read_index_page_at(input, archive_state, found->second);
    if (!page) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(page.error());
    }
    if (page.value().header.page_id != page_id) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index page id mismatch"));
    }
    if (page.value().header.page_kind == archive::kIndexPageLeaf) {
        auto leaf = index::decode_leaf_page_cbor(page.value().plaintext);
        if (!leaf) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(leaf.error());
        }
        return base::Result<std::vector<contracts::FileEntryDesc>>::success(
            std::move(leaf).value().entries);
    }
    if (page.value().header.page_kind != archive::kIndexPageInternal) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index page kind is invalid"));
    }
    auto internal = index::decode_internal_page_cbor(page.value().plaintext);
    if (!internal) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(internal.error());
    }
    std::vector<contracts::FileEntryDesc> entries;
    for (const auto child_id : internal.value().children) {
        auto child =
            collect_leaf_entries(input, archive_state, page_offsets, child_id, depth + 1);
        if (!child) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(child.error());
        }
        entries.insert(entries.end(), std::make_move_iterator(child.value().begin()),
                       std::make_move_iterator(child.value().end()));
    }
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(entries));
}

[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
load_root_leaf_entries(std::ifstream& input, const OpenedFileArchive& archive_state) {
    const auto& footer = archive_state.footer;
    if (footer.index_page_count == 0 || footer.index_root_offset == 0 ||
        footer.index_root_page_id == 0) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index root is missing"));
    }
    auto root_page =
        read_index_page_at(input, archive_state, footer.index_root_offset);
    if (!root_page) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(root_page.error());
    }
    if (root_page.value().header.page_id != footer.index_root_page_id) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index root page id mismatch"));
    }
    auto preimage = index::make_index_root_digest_preimage(
        footer.index_root_page_id, footer.index_page_count, footer.entry_count, footer.stream_count,
        root_page.value().header.content_digest);
    auto root_digest = crypto_sodium::sha256(preimage);
    if (!root_digest || root_digest.value() != footer.index_root_digest) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index root digest mismatch"));
    }
    if (footer.index_page_count == 1) {
        if (root_page.value().header.page_kind != archive::kIndexPageLeaf) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                error(base::ErrorCode::kCorruptData, "single-page index root must be a leaf"));
        }
        auto leaf = index::decode_leaf_page_cbor(root_page.value().plaintext);
        if (!leaf) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(leaf.error());
        }
        if (leaf.value().entries.size() != footer.entry_count) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                error(base::ErrorCode::kCorruptData, "file index entry count mismatch"));
        }
        return base::Result<std::vector<contracts::FileEntryDesc>>::success(
            std::move(leaf).value().entries);
    }
    auto page_offsets = map_index_page_offsets(input, archive_state);
    if (!page_offsets) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(page_offsets.error());
    }
    auto entries = collect_leaf_entries(input, archive_state, page_offsets.value(),
                                        footer.index_root_page_id, 0);
    if (!entries) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(entries.error());
    }
    if (entries.value().size() != footer.entry_count) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
            error(base::ErrorCode::kCorruptData, "file index entry count mismatch"));
    }
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(entries).value());
}

[[nodiscard]] base::Result<void>
scan_file_stream_chunks(std::ifstream& input, OpenedFileArchive& state) {
    // Stream chunks are followed by one or more index pages (leaves then optional internal root).
    // Stop at the first index page rather than at index_root_offset, which may point past leaves.
    const auto end_offset = state.footer.part_file_size - archive::kBackupFooterSize;
    auto offset = state.preamble.header.first_record_offset;
    while (offset < end_offset) {
        auto prefix_bytes = read_exact(input, offset, archive::kArchiveRecordPrefixSize);
        if (!prefix_bytes) {
            return base::Result<void>::failure(prefix_bytes.error());
        }
        auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
        if (!prefix) {
            return base::Result<void>::failure(prefix.error());
        }
        if (prefix.value().record_kind == archive::kRecordKindFileIndexPage) {
            break;
        }
        if (prefix.value().record_kind != archive::kRecordKindFileStreamChunk) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "unexpected record before file index"));
        }
        const auto header_offset = offset + archive::kArchiveRecordPrefixSize;
        auto header_bytes = read_exact(input, header_offset, archive::kFileStreamChunkHeaderSize);
        if (!header_bytes) {
            return base::Result<void>::failure(header_bytes.error());
        }
        auto header = archive::decode_file_stream_chunk_header(header_bytes.value());
        if (!header) {
            return base::Result<void>::failure(header.error());
        }
        const auto entries_offset = header_offset + archive::kFileStreamChunkHeaderSize;
        const auto entries_bytes =
            static_cast<std::uint64_t>(header.value().block_entry_count) * archive::kBlockEntrySize;
        if (entries_bytes + header.value().payload_size != prefix.value().body_size) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "file stream chunk body size is inconsistent"));
        }
        StreamChunkLocator locator;
        locator.header = header.value();
        locator.entries.reserve(header.value().block_entry_count);
        for (std::uint32_t i = 0; i < header.value().block_entry_count; ++i) {
            auto entry_bytes =
                read_exact(input, entries_offset + static_cast<std::uint64_t>(i) * archive::kBlockEntrySize,
                           archive::kBlockEntrySize);
            if (!entry_bytes) {
                return base::Result<void>::failure(entry_bytes.error());
            }
            auto entry = archive::decode_block_entry(entry_bytes.value());
            if (!entry) {
                return base::Result<void>::failure(entry.error());
            }
            locator.entries.push_back(entry.value());
        }
        locator.payload_offset = entries_offset + entries_bytes;
        if (!state.chunks_by_index.emplace(header.value().chunk_index, std::move(locator)).second) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "duplicate file stream chunk index"));
        }
        const auto record_size =
            archive::kArchiveRecordPrefixSize + archive::kFileStreamChunkHeaderSize +
            prefix.value().body_size;
        if (offset > end_offset - record_size) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "file stream chunk overflows index root"));
        }
        offset += record_size;
    }
    if (state.chunks_by_index.size() != state.footer.file_stream_chunk_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file stream chunk count mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::uint64_t> parse_token(const std::optional<std::string>& token) {
    if (!token || token->empty()) {
        return base::Result<std::uint64_t>::success(0);
    }
    std::uint64_t value = 0;
    const auto* begin = token->data();
    const auto* end = begin + token->size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
    }
    return base::Result<std::uint64_t>::success(value);
}

[[nodiscard]] bool entry_has_children(const std::vector<contracts::FileEntryDesc>& entries,
                                      const std::uint64_t entry_id) noexcept {
    return std::ranges::any_of(entries, [entry_id](const contracts::FileEntryDesc& entry) {
        return entry.parent_entry_id == entry_id;
    });
}

[[nodiscard]] contracts::RecoveryPointEntrySummary
make_summary(const contracts::FileEntryDesc& entry,
             const std::vector<contracts::FileEntryDesc>& all) {
    contracts::RecoveryPointEntrySummary summary;
    summary.entry_id = std::to_string(entry.entry_id);
    // Keep display_name empty: name is UTF-16LE customer data; Service may render later.
    summary.entry_kind = entry.kind;
    summary.logical_size_bytes = entry.logical_size;
    summary.has_children = entry_has_children(all, entry.entry_id);
    return summary;
}

} // namespace

struct PersonalFileArchiveReader::Impl final {
    OpenedFileArchive archive;
    ArchiveIdentity identity;
    mutable std::ifstream input;
};

PersonalFileArchiveReader::PersonalFileArchiveReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalFileArchiveReader::~PersonalFileArchiveReader() = default;

[[nodiscard]] base::Result<void>
install_file_ciphers(OpenedFileArchive& state, const std::string_view password) {
    if (!state.encryption_enabled) {
        return base::Result<void>::success();
    }
    if (password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "encrypted file archive requires a password"));
    }
    auto payload =
        crypto_sodium::PayloadCipher::create(password, state.preamble.kdf, state.preamble.salt);
    if (!payload) {
        return base::Result<void>::failure(payload.error());
    }
    state.payload_cipher = std::move(payload).value();
    auto index_cipher = crypto_sodium::PayloadCipher::create_index_page(
        password, state.preamble.kdf, state.preamble.salt);
    if (!index_cipher) {
        return base::Result<void>::failure(index_cipher.error());
    }
    state.index_cipher = std::move(index_cipher).value();
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
index_loaded_entries(OpenedFileArchive& state) {
    for (std::size_t i = 0; i < state.entries.size(); ++i) {
        if (!state.entry_by_id.emplace(state.entries[i].entry_id, i).second) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
    }
    return base::Result<void>::success();
}

/// V7 §5.3 / C07: unique IDs already indexed; require parent existence, directory parents,
/// no cycles, and depth ≤ product limit so restore cannot hang on a damaged parent graph.
[[nodiscard]] base::Result<void>
validate_file_index_parent_graph(const OpenedFileArchive& state) {
    const auto corrupt = []() {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
    };
    for (const auto& entry : state.entries) {
        if (entry.entry_id == 0) {
            return corrupt();
        }
        if (entry.parent_entry_id == entry.entry_id) {
            return corrupt();
        }
        if (entry.parent_entry_id == 0) {
            continue;
        }
        const auto parent_it = state.entry_by_id.find(entry.parent_entry_id);
        if (parent_it == state.entry_by_id.end()) {
            return corrupt();
        }
        if (state.entries[parent_it->second].kind != contracts::FileEntryKind::kDirectory) {
            return corrupt();
        }
    }
    for (const auto& entry : state.entries) {
        std::unordered_set<std::uint64_t> seen;
        auto current = entry.entry_id;
        std::uint32_t depth = 0;
        while (current != 0) {
            if (!seen.insert(current).second) {
                return corrupt();
            }
            ++depth;
            if (depth > contracts::kMaximumFileDirectoryDepth) {
                return corrupt();
            }
            const auto it = state.entry_by_id.find(current);
            if (it == state.entry_by_id.end()) {
                return corrupt();
            }
            current = state.entries[it->second].parent_entry_id;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<OpenedFileArchive>
open_file_archive_state(std::ifstream& input, const ArchiveOpenRequest& request,
                        const std::uint64_t file_size) {
    auto preamble = detail::read_archive_preamble(input, request, file_size);
    if (!preamble) {
        return base::Result<OpenedFileArchive>::failure(preamble.error());
    }
    if (preamble.value().header.content_kind != archive::kContentKindFileSet) {
        return base::Result<OpenedFileArchive>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive content kind is not file_set"));
    }
    OpenedFileArchive state;
    state.preamble = std::move(preamble).value();
    state.path = request.source;
    state.encryption_enabled =
        (state.preamble.header.flags & archive::kBackupFlagEncrypted) != 0;
    auto header_bytes = read_exact(input, 0, archive::kBackupHeaderSize);
    if (!header_bytes) {
        return base::Result<OpenedFileArchive>::failure(header_bytes.error());
    }
    std::copy_n(header_bytes.value().begin(), archive::kBackupHeaderSize, state.part_header.begin());
    auto footer = read_required_footer(input, file_size);
    if (!footer) {
        return base::Result<OpenedFileArchive>::failure(footer.error());
    }
    state.footer = std::move(footer).value();
    if (state.footer.file_uuid != state.preamble.header.file_uuid ||
        state.footer.volume_chunk_count != 0 || state.footer.entry_count == 0) {
        return base::Result<OpenedFileArchive>::failure(
            error(base::ErrorCode::kCorruptData, "file archive footer is inconsistent"));
    }
    auto ciphers = install_file_ciphers(state, request.password);
    if (!ciphers) {
        return base::Result<OpenedFileArchive>::failure(ciphers.error());
    }
    auto entries = load_root_leaf_entries(input, state);
    if (!entries) {
        return base::Result<OpenedFileArchive>::failure(entries.error());
    }
    state.entries = std::move(entries).value();
    auto indexed = index_loaded_entries(state);
    if (!indexed) {
        return base::Result<OpenedFileArchive>::failure(indexed.error());
    }
    auto graph = validate_file_index_parent_graph(state);
    if (!graph) {
        return base::Result<OpenedFileArchive>::failure(graph.error());
    }
    auto scanned = scan_file_stream_chunks(input, state);
    if (!scanned) {
        return base::Result<OpenedFileArchive>::failure(scanned.error());
    }
    return base::Result<OpenedFileArchive>::success(std::move(state));
}

base::Result<std::unique_ptr<PersonalFileArchiveReader>>
PersonalFileArchiveReader::open(const ArchiveOpenRequest& request) {
    if (request.source.empty()) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive source is required"));
    }
    std::ifstream input(request.source, std::ios::binary);
    if (!input) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(
            error(base::ErrorCode::kNotFound, "file archive was not found"));
    }
    auto file_size = read_stream_size(input);
    if (!file_size) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(file_size.error());
    }
    auto state = open_file_archive_state(input, request, file_size.value());
    if (!state) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(state.error());
    }
    auto implementation = std::make_unique<Impl>();
    implementation->identity.file_uuid = state.value().preamble.header.file_uuid;
    implementation->identity.backup_set_uuid = state.value().preamble.header.backup_set_uuid;
    implementation->identity.parent_uuid = state.value().preamble.header.parent_uuid;
    implementation->identity.backup_type =
        detail::archive_backup_type(state.value().preamble.header);
    implementation->identity.block_size = state.value().preamble.header.block_size;
    implementation->input = std::move(input);
    implementation->archive = std::move(state).value();
    return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::success(
        std::unique_ptr<PersonalFileArchiveReader>(
            new PersonalFileArchiveReader(std::move(implementation))));
}

const format::Manifest& PersonalFileArchiveReader::manifest() const noexcept {
    return implementation_->archive.preamble.manifest;
}

const ArchiveIdentity& PersonalFileArchiveReader::identity() const noexcept {
    return implementation_->identity;
}

std::string PersonalFileArchiveReader::index_root_digest() const {
    return digest_to_hex(implementation_->archive.footer.index_root_digest);
}

std::uint64_t PersonalFileArchiveReader::entry_count() const noexcept {
    return implementation_->archive.footer.entry_count;
}

std::uint64_t PersonalFileArchiveReader::stream_count() const noexcept {
    return implementation_->archive.footer.stream_count;
}

base::Result<ports::FileEntryPage>
PersonalFileArchiveReader::list_children(const std::uint64_t parent_entry_id,
                                         const std::uint32_t maximum_results,
                                         const std::optional<std::string>& continuation_token,
                                         const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<ports::FileEntryPage>::failure(
            error(base::ErrorCode::kCancelled, "list cancelled"));
    }
    if (maximum_results == 0) {
        return base::Result<ports::FileEntryPage>::failure(
            error(base::ErrorCode::kInvalidArgument, "maximum_results must be positive"));
    }
    auto start = parse_token(continuation_token);
    if (!start) {
        return base::Result<ports::FileEntryPage>::failure(start.error());
    }
    ports::FileEntryPage page;
    std::uint64_t matched = 0;
    for (const auto& entry : implementation_->archive.entries) {
        if (entry.parent_entry_id != parent_entry_id) {
            continue;
        }
        if (matched < start.value()) {
            ++matched;
            continue;
        }
        if (page.items.size() >= maximum_results) {
            page.continuation_token = std::to_string(matched);
            break;
        }
        page.items.push_back(make_summary(entry, implementation_->archive.entries));
        ++matched;
    }
    return base::Result<ports::FileEntryPage>::success(std::move(page));
}

base::Result<contracts::FileEntryDesc>
PersonalFileArchiveReader::describe_entry(const std::uint64_t entry_id,
                                          const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kCancelled, "describe cancelled"));
    }
    const auto found = implementation_->archive.entry_by_id.find(entry_id);
    if (found == implementation_->archive.entry_by_id.end()) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kNotFound, "file entry was not found"));
    }
    return base::Result<contracts::FileEntryDesc>::success(
        implementation_->archive.entries[found->second]);
}

base::Result<std::size_t>
PersonalFileArchiveReader::read_stream(const ports::FileStreamReadRequest& request,
                                       const std::span<std::byte> destination,
                                       const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kCancelled, "stream read cancelled"));
    }
    if (request.stream_index == 0 || request.size == 0 || destination.size() < request.size) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream read request is invalid"));
    }
    const contracts::FileStreamDesc* stream = nullptr;
    for (const auto& entry : implementation_->archive.entries) {
        for (const auto& candidate : entry.streams) {
            if (candidate.stream_index == request.stream_index) {
                stream = &candidate;
                break;
            }
        }
        if (stream != nullptr) {
            break;
        }
    }
    if (stream == nullptr) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kNotFound, "file stream was not found"));
    }
    if (request.offset >= stream->logical_size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto readable =
        (std::min)(request.size, static_cast<std::uint64_t>(stream->logical_size - request.offset));
    std::size_t written = 0;
    const auto end = request.offset + readable;
    for (const auto& extent : stream->extents) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCancelled, "stream read cancelled"));
        }
        const auto extent_end = extent.file_offset + extent.logical_size;
        if (extent_end <= request.offset || extent.file_offset >= end) {
            continue;
        }
        const auto chunk_it = implementation_->archive.chunks_by_index.find(extent.chunk_index);
        if (chunk_it == implementation_->archive.chunks_by_index.end()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "stream extent references missing chunk"));
        }
        const auto& locator = chunk_it->second;
        if (extent.block_entry_index >= locator.entries.size()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "stream extent block index is invalid"));
        }
        // F2 writer emits one block entry per chunk; payload is the full block payload.
        auto payload_bytes =
            read_exact(implementation_->input, locator.payload_offset, locator.header.payload_size);
        if (!payload_bytes) {
            return base::Result<std::size_t>::failure(payload_bytes.error());
        }
        std::vector<std::byte> plaintext = std::move(payload_bytes).value();
        if (implementation_->archive.encryption_enabled) {
            auto aad = make_file_chunk_aad(implementation_->archive.part_header,
                                           archive::kBlockEntrySize + plaintext.size(),
                                           locator.header, locator.entries);
            if (!aad) {
                return base::Result<std::size_t>::failure(aad.error());
            }
            auto plain = implementation_->archive.payload_cipher->unprotect(
                plaintext, aad.value(), locator.header.payload_nonce,
                locator.header.payload_authentication_tag);
            if (!plain) {
                return base::Result<std::size_t>::failure(plain.error());
            }
            plaintext = std::move(plain).value();
        }
        const auto copy_begin = (std::max)(request.offset, extent.file_offset);
        const auto copy_end = (std::min)(end, extent_end);
        const auto local_begin = static_cast<std::size_t>(copy_begin - extent.file_offset);
        const auto local_size = static_cast<std::size_t>(copy_end - copy_begin);
        if (local_begin + local_size > plaintext.size()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "stream extent exceeds chunk payload"));
        }
        const auto dest_offset = static_cast<std::size_t>(copy_begin - request.offset);
        std::copy_n(plaintext.begin() + static_cast<std::ptrdiff_t>(local_begin), local_size,
                    destination.begin() + static_cast<std::ptrdiff_t>(dest_offset));
        written = (std::max)(written, dest_offset + local_size);
    }
    return base::Result<std::size_t>::success(written);
}

} // namespace aegra::adapters::personal_archive
