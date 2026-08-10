#include "personal_file_archive_secondary_index.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"
#include "aegra/format/file_index.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <span>
#include <string>
#include <utility>

namespace aegra::adapters::personal_archive::secondary_index {
namespace {

namespace archive = format::personal_archive;
namespace index = format::file_index;

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] base::Result<void> write_bytes(detail::Win32OutputFile& output,
                                             const std::span<const std::byte> bytes) {
    return output.write(bytes);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
make_index_page_aad(const archive::EncodedBackupHeader& part_header,
                    const std::size_t ciphertext_size, archive::FileIndexPageHeader header) {
    header.authentication_tag = {};
    header.content_digest = {};
    auto encoded = archive::encode_file_index_page_header(header);
    if (!encoded) {
        return base::Result<std::vector<std::byte>>::failure(encoded.error());
    }
    std::vector<std::byte> aad;
    aad.reserve(part_header.size() + archive::kArchiveRecordPrefixSize +
                archive::kFileIndexPageHeaderSize);
    aad.insert(aad.end(), part_header.begin(), part_header.end());
    const auto prefix =
        archive::make_file_index_page_record_prefix(ciphertext_size);
    auto encoded_prefix = archive::encode_archive_record_prefix(prefix);
    if (!encoded_prefix) {
        return base::Result<std::vector<std::byte>>::failure(encoded_prefix.error());
    }
    aad.insert(aad.end(), encoded_prefix.value().begin(), encoded_prefix.value().end());
    aad.insert(aad.end(), encoded.value().begin(), encoded.value().end());
    return base::Result<std::vector<std::byte>>::success(std::move(aad));
}

struct PreparedPage final {
    archive::FileIndexPageHeader header;
    std::vector<std::byte> ciphertext;
};

[[nodiscard]] base::Result<PreparedPage>
protect_page(PreparedPage prepared, const bool encryption_enabled,
             crypto_sodium::PayloadCipher* payload_cipher,
             const archive::EncodedBackupHeader& part_header) {
    if (prepared.ciphertext.empty() ||
        prepared.ciphertext.size() > archive::kMaximumIndexPagePlainBytes) {
        return base::Result<PreparedPage>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
    }
    prepared.header.plain_size = static_cast<std::uint32_t>(prepared.ciphertext.size());
    prepared.header.encoded_size = prepared.header.plain_size;
    auto nonce = crypto_sodium::create_payload_nonce();
    if (!nonce) {
        return base::Result<PreparedPage>::failure(nonce.error());
    }
    prepared.header.nonce = nonce.value();
    if (!encryption_enabled) {
        return base::Result<PreparedPage>::success(std::move(prepared));
    }
    if (payload_cipher == nullptr) {
        return base::Result<PreparedPage>::failure(
            error(base::ErrorCode::kInternal, "file archive index cipher is missing"));
    }
    auto aad = make_index_page_aad(part_header, prepared.ciphertext.size(), prepared.header);
    if (!aad) {
        return base::Result<PreparedPage>::failure(aad.error());
    }
    auto protected_page =
        payload_cipher->protect(prepared.ciphertext, aad.value(), prepared.header.nonce);
    if (!protected_page) {
        return base::Result<PreparedPage>::failure(protected_page.error());
    }
    prepared.ciphertext = std::move(protected_page.value().ciphertext);
    prepared.header.authentication_tag = protected_page.value().tag;
    prepared.header.encoded_size = static_cast<std::uint32_t>(prepared.ciphertext.size());
    return base::Result<PreparedPage>::success(std::move(prepared));
}

[[nodiscard]] base::Result<void>
emit_page(detail::Win32OutputFile& output, const archive::FileIndexPageHeader& header,
          const std::span<const std::byte> ciphertext) {
    auto prefix = archive::encode_archive_record_prefix(
        archive::make_file_index_page_record_prefix(ciphertext.size()));
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

struct BuiltPage final {
    std::uint64_t page_id{0};
    std::uint64_t first_key{0};
    std::array<std::byte, 32> content_digest{};
    std::uint64_t file_offset{0};
};

[[nodiscard]] base::Result<PreparedPage>
prepare_page(const std::uint16_t page_kind, const std::vector<std::byte>& plain,
             const std::uint64_t page_id, const std::uint32_t entry_count,
             const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
             const archive::EncodedBackupHeader& part_header) {
    auto digest = crypto_sodium::sha256(plain);
    if (!digest) {
        return base::Result<PreparedPage>::failure(digest.error());
    }
    PreparedPage prepared;
    prepared.header.page_kind = page_kind;
    prepared.header.page_id = page_id;
    prepared.header.entry_count = entry_count;
    prepared.header.content_digest = digest.value();
    prepared.ciphertext = plain;
    return protect_page(std::move(prepared), encryption_enabled, index_cipher, part_header);
}

template <typename Record>
[[nodiscard]] base::Result<std::vector<BuiltPage>>
write_sorted_leaves(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                    const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                    const archive::EncodedBackupHeader& part_header,
                    const std::uint16_t leaf_kind, const std::vector<Record>& records,
                    base::Result<std::vector<std::byte>> (*encode)(const std::vector<Record>&),
                    std::uint64_t (*first_key)(const Record&)) {
    if (records.empty()) {
        return base::Result<std::vector<BuiltPage>>::failure(
            error(base::ErrorCode::kInvalidArgument, "secondary index requires records"));
    }
    std::vector<BuiltPage> leaves;
    std::size_t index = 0;
    while (index < records.size()) {
        std::size_t take = 1;
        while (index + take < records.size() &&
               take < static_cast<std::size_t>(index::kMaximumLeafEntriesPerPage)) {
            std::vector<Record> trial(
                records.begin() + static_cast<std::ptrdiff_t>(index),
                records.begin() + static_cast<std::ptrdiff_t>(index + take + 1));
            auto plain = encode(trial);
            if (!plain) {
                return base::Result<std::vector<BuiltPage>>::failure(plain.error());
            }
            if (plain.value().size() > archive::kMaximumIndexPagePlainBytes) {
                break;
            }
            ++take;
        }
        std::vector<Record> group(records.begin() + static_cast<std::ptrdiff_t>(index),
                                  records.begin() + static_cast<std::ptrdiff_t>(index + take));
        auto plain = encode(group);
        if (!plain || plain.value().size() > archive::kMaximumIndexPagePlainBytes) {
            return base::Result<std::vector<BuiltPage>>::failure(
                !plain ? plain.error()
                       : error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
        const auto page_id = next_page_id++;
        auto prepared =
            prepare_page(leaf_kind, plain.value(), page_id, static_cast<std::uint32_t>(group.size()),
                         encryption_enabled, index_cipher, part_header);
        if (!prepared) {
            return base::Result<std::vector<BuiltPage>>::failure(prepared.error());
        }
        const auto page_offset = output.position();
        auto written = emit_page(output, prepared.value().header, prepared.value().ciphertext);
        if (!written) {
            return base::Result<std::vector<BuiltPage>>::failure(written.error());
        }
        BuiltPage built;
        built.page_id = page_id;
        built.first_key = first_key(group.front());
        built.content_digest = prepared.value().header.content_digest;
        built.file_offset = page_offset;
        leaves.push_back(std::move(built));
        index += take;
    }
    return base::Result<std::vector<BuiltPage>>::success(std::move(leaves));
}

[[nodiscard]] base::Result<index::IntegerInternalPageBody>
make_internal(const std::uint16_t page_kind, const std::span<const BuiltPage> children) {
    if (children.size() < 2 ||
        children.size() > static_cast<std::size_t>(index::kMaximumInternalKeysPerPage) + 1U) {
        return base::Result<index::IntegerInternalPageBody>::failure(
            error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
    }
    index::IntegerInternalPageBody body;
    body.page_kind = page_kind;
    for (const auto& child : children) {
        body.children.push_back(index::ChildPageLocator{child.page_id, child.file_offset});
    }
    for (std::size_t i = 1; i < children.size(); ++i) {
        body.keys.push_back(children[i].first_key);
    }
    return base::Result<index::IntegerInternalPageBody>::success(std::move(body));
}

[[nodiscard]] base::Result<BuiltPage>
write_internal_group(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                     const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const archive::EncodedBackupHeader& part_header,
                     const std::uint16_t internal_kind, const std::span<const BuiltPage> children) {
    auto body = make_internal(internal_kind, children);
    if (!body) {
        return base::Result<BuiltPage>::failure(body.error());
    }
    auto plain = index::encode_integer_internal_page_cbor(body.value());
    if (!plain) {
        return base::Result<BuiltPage>::failure(plain.error());
    }
    const auto page_id = next_page_id++;
    auto prepared =
        prepare_page(internal_kind, plain.value(), page_id,
                     static_cast<std::uint32_t>(body.value().keys.size()), encryption_enabled,
                     index_cipher, part_header);
    if (!prepared) {
        return base::Result<BuiltPage>::failure(prepared.error());
    }
    const auto page_offset = output.position();
    auto written = emit_page(output, prepared.value().header, prepared.value().ciphertext);
    if (!written) {
        return base::Result<BuiltPage>::failure(written.error());
    }
    BuiltPage built;
    built.page_id = page_id;
    built.first_key = children.front().first_key;
    built.content_digest = prepared.value().header.content_digest;
    built.file_offset = page_offset;
    return base::Result<BuiltPage>::success(std::move(built));
}

[[nodiscard]] base::Result<TreeWriteResult>
raise_and_hash(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
               const bool encryption_enabled,
               crypto_sodium::PayloadCipher* index_cipher,
               const archive::EncodedBackupHeader& part_header, const std::string_view digest_label,
               const std::uint64_t primary_count, const std::uint16_t internal_kind,
               std::vector<BuiltPage> level) {
    if (level.empty()) {
        return base::Result<TreeWriteResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "secondary index requires leaves"));
    }
    std::uint64_t page_count = level.size();
    std::uint32_t levels = 0;
    while (level.size() > 1) {
        ++levels;
        if (levels > index::kMaximumIndexDepth) {
            return base::Result<TreeWriteResult>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_depth_limit"));
        }
        std::vector<BuiltPage> parents;
        std::size_t start = 0;
        while (start < level.size()) {
            auto remaining = level.size() - start;
            auto take = (std::min)(remaining,
                                   static_cast<std::size_t>(index::kMaximumInternalKeysPerPage) + 1U);
            if (remaining > take && remaining - take == 1) {
                --take;
            }
            while (take >= 2) {
                auto body =
                    make_internal(internal_kind, std::span<const BuiltPage>(level.data() + start, take));
                if (!body) {
                    --take;
                    continue;
                }
                auto plain = index::encode_integer_internal_page_cbor(body.value());
                if (!plain) {
                    return base::Result<TreeWriteResult>::failure(plain.error());
                }
                if (plain.value().size() <= archive::kMaximumIndexPagePlainBytes) {
                    break;
                }
                --take;
            }
            if (take < 2) {
                return base::Result<TreeWriteResult>::failure(
                    error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
            }
            auto written =
                write_internal_group(output, next_page_id, encryption_enabled, index_cipher,
                                     part_header, internal_kind,
                                     std::span<const BuiltPage>(level.data() + start, take));
            if (!written) {
                return base::Result<TreeWriteResult>::failure(written.error());
            }
            parents.push_back(std::move(written).value());
            start += take;
        }
        if (parents.empty() || parents.size() >= level.size()) {
            return base::Result<TreeWriteResult>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_depth_limit"));
        }
        if (page_count > (std::numeric_limits<std::uint64_t>::max)() - parents.size()) {
            return base::Result<TreeWriteResult>::failure(
                error(base::ErrorCode::kInvalidArgument, "file_backup.index_page_limit"));
        }
        page_count += parents.size();
        level = std::move(parents);
    }
    // page_count omitted from preimage (0): only total index_page_count is in Footer.
    auto preimage = index::make_index_root_digest_preimage(
        digest_label, level.front().page_id, 0, primary_count, 0, level.front().content_digest);
    auto digest = crypto_sodium::sha256(preimage);
    if (!digest) {
        return base::Result<TreeWriteResult>::failure(digest.error());
    }
    TreeWriteResult result;
    result.locator.page_id = level.front().page_id;
    result.locator.offset = level.front().file_offset;
    result.locator.digest = digest.value();
    result.page_count = page_count;
    return base::Result<TreeWriteResult>::success(result);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_entry_leaf(const std::vector<index::EntryIdIndexRecord>& records) {
    index::EntryIdLeafPageBody body;
    body.records = records;
    return index::encode_entry_id_leaf_page_cbor(body);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_stream_leaf(const std::vector<index::StreamIndexRecord>& records) {
    index::StreamLeafPageBody body;
    body.records = records;
    return index::encode_stream_leaf_page_cbor(body);
}

[[nodiscard]] base::Result<std::vector<std::byte>>
encode_chunk_leaf(const std::vector<index::ChunkIndexRecord>& records) {
    index::ChunkLeafPageBody body;
    body.records = records;
    return index::encode_chunk_leaf_page_cbor(body);
}

[[nodiscard]] std::uint64_t entry_key(const index::EntryIdIndexRecord& rec) {
    return rec.entry_id;
}
[[nodiscard]] std::uint64_t stream_key(const index::StreamIndexRecord& rec) {
    return rec.stream_index;
}
[[nodiscard]] std::uint64_t chunk_key(const index::ChunkIndexRecord& rec) {
    return rec.chunk_index;
}

} // namespace

base::Result<TreeWriteResult>
write_entry_id_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                     const bool encryption_enabled, crypto_sodium::PayloadCipher* index_cipher,
                     const archive::EncodedBackupHeader& part_header,
                     std::vector<index::EntryIdIndexRecord> records) {
    std::ranges::sort(records, [](const auto& left, const auto& right) {
        return left.entry_id < right.entry_id;
    });
    if (std::adjacent_find(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.entry_id == right.entry_id;
        }) != records.end()) {
        return base::Result<TreeWriteResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "duplicate entry id index key"));
    }
    auto leaves =
        write_sorted_leaves(output, next_page_id, encryption_enabled, index_cipher, part_header,
                            index::kPageKindEntryIdLeaf, records, encode_entry_leaf, entry_key);
    if (!leaves) {
        return base::Result<TreeWriteResult>::failure(leaves.error());
    }
    return raise_and_hash(output, next_page_id, encryption_enabled, index_cipher, part_header,
                          index::kEntryIdIndexRootDigestLabel, records.size(),
                          index::kPageKindEntryIdInternal, std::move(leaves).value());
}

base::Result<TreeWriteResult>
write_stream_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                   const bool encryption_enabled,
                   crypto_sodium::PayloadCipher* index_cipher,
                   const archive::EncodedBackupHeader& part_header,
                   std::vector<index::StreamIndexRecord> records) {
    std::ranges::sort(records, [](const auto& left, const auto& right) {
        return left.stream_index < right.stream_index;
    });
    if (std::adjacent_find(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.stream_index == right.stream_index;
        }) != records.end()) {
        return base::Result<TreeWriteResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "duplicate stream index key"));
    }
    auto leaves =
        write_sorted_leaves(output, next_page_id, encryption_enabled, index_cipher, part_header,
                            index::kPageKindStreamLeaf, records, encode_stream_leaf, stream_key);
    if (!leaves) {
        return base::Result<TreeWriteResult>::failure(leaves.error());
    }
    return raise_and_hash(output, next_page_id, encryption_enabled, index_cipher, part_header,
                          index::kStreamIndexRootDigestLabel, records.size(),
                          index::kPageKindStreamInternal, std::move(leaves).value());
}

base::Result<TreeWriteResult>
write_chunk_index(detail::Win32OutputFile& output, std::uint64_t& next_page_id,
                  const bool encryption_enabled,
                  crypto_sodium::PayloadCipher* index_cipher,
                  const archive::EncodedBackupHeader& part_header,
                  std::vector<index::ChunkIndexRecord> records) {
    std::ranges::sort(records, [](const auto& left, const auto& right) {
        return left.chunk_index < right.chunk_index;
    });
    if (std::adjacent_find(records.begin(), records.end(), [](const auto& left, const auto& right) {
            return left.chunk_index == right.chunk_index;
        }) != records.end()) {
        return base::Result<TreeWriteResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "duplicate chunk index key"));
    }
    auto leaves =
        write_sorted_leaves(output, next_page_id, encryption_enabled, index_cipher, part_header,
                            index::kPageKindChunkLeaf, records, encode_chunk_leaf, chunk_key);
    if (!leaves) {
        return base::Result<TreeWriteResult>::failure(leaves.error());
    }
    return raise_and_hash(output, next_page_id, encryption_enabled, index_cipher, part_header,
                          index::kChunkIndexRootDigestLabel, records.size(),
                          index::kPageKindChunkInternal, std::move(leaves).value());
}

} // namespace aegra::adapters::personal_archive::secondary_index
