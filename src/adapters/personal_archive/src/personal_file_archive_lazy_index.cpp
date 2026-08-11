#include "personal_file_archive_lazy_index.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <unordered_set>
#include <utility>

namespace aegra::adapters::personal_archive::lazy_index {
namespace {

namespace archive = format::personal_archive;
namespace index = format::file_index;

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] char* as_mutable_chars(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
    return reinterpret_cast<char*>(value);
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

[[nodiscard]] base::Result<DecodedPage>
read_index_page_uncached(std::ifstream& input, const OpenedFileArchive& state,
                         const std::uint64_t page_offset) {
    auto prefix_bytes = read_exact(input, page_offset, archive::kArchiveRecordPrefixSize);
    if (!prefix_bytes) {
        return base::Result<DecodedPage>::failure(prefix_bytes.error());
    }
    auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
    if (!prefix || prefix.value().record_kind != archive::kRecordKindFileIndexPage) {
        return base::Result<DecodedPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page record is invalid"));
    }
    auto header_bytes = read_exact(input, page_offset + archive::kArchiveRecordPrefixSize,
                                   archive::kFileIndexPageHeaderSize);
    if (!header_bytes) {
        return base::Result<DecodedPage>::failure(header_bytes.error());
    }
    auto page_header = archive::decode_file_index_page_header(header_bytes.value());
    if (!page_header) {
        return base::Result<DecodedPage>::failure(page_header.error());
    }
    if (page_header.value().encoded_size > archive::kMaximumIndexPagePlainBytes ||
        page_header.value().plain_size > archive::kMaximumIndexPagePlainBytes ||
        prefix.value().body_size != page_header.value().encoded_size) {
        return base::Result<DecodedPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page header is invalid"));
    }
    const auto body_offset = page_offset + archive::kFileIndexPageRecordHeaderSize;
    auto body = read_exact(input, body_offset, page_header.value().encoded_size);
    if (!body) {
        return base::Result<DecodedPage>::failure(body.error());
    }
    std::vector<std::byte> plaintext = std::move(body).value();
    if (state.encryption_enabled) {
        auto aad = make_index_page_aad(state.part_header, plaintext.size(), page_header.value());
        if (!aad) {
            return base::Result<DecodedPage>::failure(aad.error());
        }
        auto plain = state.index_cipher->unprotect(plaintext, aad.value(), page_header.value().nonce,
                                                   page_header.value().authentication_tag);
        if (!plain) {
            return base::Result<DecodedPage>::failure(plain.error());
        }
        plaintext = std::move(plain).value();
    }
    if (plaintext.size() != page_header.value().plain_size) {
        return base::Result<DecodedPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page plain size mismatch"));
    }
    auto digest = crypto_sodium::sha256(plaintext);
    if (!digest || digest.value() != page_header.value().content_digest) {
        return base::Result<DecodedPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page content digest mismatch"));
    }
    DecodedPage decoded;
    decoded.header = std::move(page_header).value();
    decoded.plaintext = std::move(plaintext);
    decoded.offset = page_offset;
    return base::Result<DecodedPage>::success(std::move(decoded));
}

[[nodiscard]] base::Result<DecodedPage>
get_page_cached(std::ifstream& input, OpenedFileArchive& state, const std::uint64_t page_offset,
                const std::uint64_t expected_page_id) {
    auto& cache = state.page_cache;
    for (auto& slot : cache.slots) {
        if (slot.occupied && slot.offset == page_offset) {
            if (expected_page_id != 0 && slot.page_id != expected_page_id) {
                return base::Result<DecodedPage>::failure(
                    error(base::ErrorCode::kCorruptData, "file index page id mismatch"));
            }
            slot.tick = ++cache.clock;
            DecodedPage page;
            page.header = slot.header;
            page.plaintext = slot.plaintext;
            page.offset = slot.offset;
            return base::Result<DecodedPage>::success(std::move(page));
        }
    }
    auto loaded = read_index_page_uncached(input, state, page_offset);
    if (!loaded) {
        return loaded;
    }
    if (expected_page_id != 0 && loaded.value().header.page_id != expected_page_id) {
        return base::Result<DecodedPage>::failure(
            error(base::ErrorCode::kCorruptData, "file index page id mismatch"));
    }
    // Evict lowest tick (or empty).
    std::size_t victim = 0;
    std::uint32_t best_tick = (std::numeric_limits<std::uint32_t>::max)();
    for (std::size_t i = 0; i < cache.slots.size(); ++i) {
        if (!cache.slots[i].occupied) {
            victim = i;
            break;
        }
        if (cache.slots[i].tick < best_tick) {
            best_tick = cache.slots[i].tick;
            victim = i;
        }
    }
    auto& slot = cache.slots[victim];
    slot.occupied = true;
    slot.offset = loaded.value().offset;
    slot.page_id = loaded.value().header.page_id;
    slot.header = loaded.value().header;
    slot.plaintext = loaded.value().plaintext;
    slot.tick = ++cache.clock;
    return loaded;
}

[[nodiscard]] base::Result<void>
verify_root_page(std::ifstream& input, OpenedFileArchive& state,
                 const archive::IndexRootLocator& root, const std::string_view digest_label,
                 const std::uint64_t primary_count, const std::uint64_t secondary_count,
                 const bool required) {
    if (root.page_id == 0 && root.offset == 0) {
        if (required) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "file index root is missing"));
        }
        return base::Result<void>::success();
    }
    if (root.page_id == 0 || root.offset == 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index root locator is incomplete"));
    }
    auto page = get_page_cached(input, state, root.offset, root.page_id);
    if (!page) {
        return base::Result<void>::failure(page.error());
    }
    auto preimage = index::make_index_root_digest_preimage(
        digest_label, root.page_id, 0, primary_count, secondary_count, page.value().header.content_digest);
    auto digest = crypto_sodium::sha256(preimage);
    if (!digest || digest.value() != root.digest) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index root digest mismatch"));
    }
    return base::Result<void>::success();
}

struct NamespacePathFrame final {
    std::vector<index::ChildPageLocator> children;
    std::size_t child_index{0};
};

struct NamespaceCursor final {
    PageRef leaf;
    std::vector<NamespacePathFrame> path;
};

[[nodiscard]] base::Result<NamespaceCursor>
seek_namespace(std::ifstream& input, OpenedFileArchive& state, const index::IndexKey& target) {
    PageRef cur{state.footer.index_root_page_id, state.footer.index_root_offset};
    NamespaceCursor cursor;
    for (std::uint32_t depth = 0; depth <= index::kMaximumIndexDepth; ++depth) {
        auto page = get_page_cached(input, state, cur.offset, cur.page_id);
        if (!page) {
            return base::Result<NamespaceCursor>::failure(page.error());
        }
        if (page.value().header.page_kind == archive::kIndexPageLeaf) {
            cursor.leaf = cur;
            return base::Result<NamespaceCursor>::success(std::move(cursor));
        }
        if (page.value().header.page_kind != archive::kIndexPageInternal) {
            return base::Result<NamespaceCursor>::failure(
                error(base::ErrorCode::kCorruptData, "file index page kind is invalid"));
        }
        auto internal = index::decode_internal_page_cbor(page.value().plaintext);
        if (!internal) {
            return base::Result<NamespaceCursor>::failure(internal.error());
        }
        // Separator keys[i] = first key of children[i+1]; left child holds keys < separator.
        std::size_t child_index = 0;
        while (child_index < internal.value().keys.size() &&
               index::compare_index_keys(internal.value().keys[child_index], target) <= 0) {
            ++child_index;
        }
        if (child_index >= internal.value().children.size()) {
            return base::Result<NamespaceCursor>::failure(
                error(base::ErrorCode::kCorruptData, "file index internal child is missing"));
        }
        NamespacePathFrame frame;
        frame.children = internal.value().children;
        frame.child_index = child_index;
        cursor.path.push_back(std::move(frame));
        cur.page_id = internal.value().children[child_index].page_id;
        cur.offset = internal.value().children[child_index].offset;
    }
    return base::Result<NamespaceCursor>::failure(
        error(base::ErrorCode::kCorruptData, "file_backup.index_depth_limit"));
}

[[nodiscard]] base::Result<bool>
advance_namespace(std::ifstream& input, OpenedFileArchive& state, NamespaceCursor& cursor) {
    while (!cursor.path.empty()) {
        auto& frame = cursor.path.back();
        if (frame.child_index + 1 >= frame.children.size()) {
            cursor.path.pop_back();
            continue;
        }
        ++frame.child_index;
        auto child = frame.children[frame.child_index];
        for (std::uint32_t depth = static_cast<std::uint32_t>(cursor.path.size());
             depth <= index::kMaximumIndexDepth; ++depth) {
            auto page = get_page_cached(input, state, child.offset, child.page_id);
            if (!page) {
                return base::Result<bool>::failure(page.error());
            }
            if (page.value().header.page_kind == archive::kIndexPageLeaf) {
                cursor.leaf = PageRef{child.page_id, child.offset};
                return base::Result<bool>::success(true);
            }
            if (page.value().header.page_kind != archive::kIndexPageInternal) {
                return base::Result<bool>::failure(
                    error(base::ErrorCode::kCorruptData, "file index page kind is invalid"));
            }
            auto internal = index::decode_internal_page_cbor(page.value().plaintext);
            if (!internal) {
                return base::Result<bool>::failure(internal.error());
            }
            NamespacePathFrame next;
            next.children = internal.value().children;
            cursor.path.push_back(std::move(next));
            child = cursor.path.back().children.front();
        }
        return base::Result<bool>::failure(
            error(base::ErrorCode::kCorruptData, "file_backup.index_depth_limit"));
    }
    return base::Result<bool>::success(false);
}

[[nodiscard]] base::Result<PageRef>
navigate_namespace(std::ifstream& input, OpenedFileArchive& state, const index::IndexKey& target) {
    auto cursor = seek_namespace(input, state, target);
    if (!cursor) {
        return base::Result<PageRef>::failure(cursor.error());
    }
    return base::Result<PageRef>::success(cursor.value().leaf);
}

[[nodiscard]] base::Result<void>
collect_index_pages(std::ifstream& input, OpenedFileArchive& state, const PageRef node,
                    const std::uint16_t leaf_kind, const std::uint16_t internal_kind,
                    const std::uint32_t depth, std::unordered_set<std::uint64_t>& page_ids) {
    if (depth > index::kMaximumIndexDepth || !page_ids.insert(node.page_id).second) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
    }
    auto page = get_page_cached(input, state, node.offset, node.page_id);
    if (!page) {
        return base::Result<void>::failure(page.error());
    }
    if (page.value().header.page_kind == leaf_kind) {
        return base::Result<void>::success();
    }
    if (page.value().header.page_kind != internal_kind) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index page kind is invalid"));
    }
    std::vector<index::ChildPageLocator> children;
    if (internal_kind == archive::kIndexPageInternal) {
        auto body = index::decode_internal_page_cbor(page.value().plaintext);
        if (!body) {
            return base::Result<void>::failure(body.error());
        }
        children = std::move(body).value().children;
    } else {
        auto body = index::decode_integer_internal_page_cbor(page.value().plaintext);
        if (!body) {
            return base::Result<void>::failure(body.error());
        }
        if (body.value().page_kind != internal_kind) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "secondary index page kind mismatch"));
        }
        children = std::move(body).value().children;
    }
    for (const auto& child : children) {
        auto collected = collect_index_pages(input, state, PageRef{child.page_id, child.offset},
                                             leaf_kind, internal_kind, depth + 1, page_ids);
        if (!collected) {
            return collected;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
verify_index_page_count(std::ifstream& input, OpenedFileArchive& state) {
    std::unordered_set<std::uint64_t> page_ids;
    const std::array trees{
        std::array<std::uint64_t, 4>{state.footer.index_root_page_id,
                                     state.footer.index_root_offset, archive::kIndexPageLeaf,
                                     archive::kIndexPageInternal},
        std::array<std::uint64_t, 4>{state.footer.entry_id_root.page_id,
                                     state.footer.entry_id_root.offset,
                                     index::kPageKindEntryIdLeaf,
                                     index::kPageKindEntryIdInternal},
        std::array<std::uint64_t, 4>{state.footer.stream_root.page_id,
                                     state.footer.stream_root.offset,
                                     index::kPageKindStreamLeaf,
                                     index::kPageKindStreamInternal},
        std::array<std::uint64_t, 4>{state.footer.chunk_root.page_id,
                                     state.footer.chunk_root.offset,
                                     index::kPageKindChunkLeaf,
                                     index::kPageKindChunkInternal}};
    for (const auto& tree : trees) {
        if (tree[0] == 0) {
            continue;
        }
        auto collected = collect_index_pages(input, state, PageRef{tree[0], tree[1]},
                                             static_cast<std::uint16_t>(tree[2]),
                                             static_cast<std::uint16_t>(tree[3]), 0, page_ids);
        if (!collected) {
            return collected;
        }
    }
    if (page_ids.size() != state.footer.index_page_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index page count mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<PageRef>
navigate_integer(std::ifstream& input, OpenedFileArchive& state, const PageRef& root,
                 const std::uint64_t target_key, const std::uint16_t leaf_kind,
                 const std::uint16_t internal_kind) {
    PageRef cur = root;
    for (std::uint32_t depth = 0; depth <= index::kMaximumIndexDepth; ++depth) {
        auto page = get_page_cached(input, state, cur.offset, cur.page_id);
        if (!page) {
            return base::Result<PageRef>::failure(page.error());
        }
        if (page.value().header.page_kind == leaf_kind) {
            return base::Result<PageRef>::success(cur);
        }
        if (page.value().header.page_kind != internal_kind) {
            return base::Result<PageRef>::failure(
                error(base::ErrorCode::kCorruptData, "secondary index page kind is invalid"));
        }
        auto internal = index::decode_integer_internal_page_cbor(page.value().plaintext);
        if (!internal) {
            return base::Result<PageRef>::failure(internal.error());
        }
        std::size_t child_index = 0;
        while (child_index < internal.value().keys.size() &&
               internal.value().keys[child_index] <= target_key) {
            ++child_index;
        }
        if (child_index >= internal.value().children.size()) {
            return base::Result<PageRef>::failure(
                error(base::ErrorCode::kCorruptData, "secondary index child is missing"));
        }
        cur.page_id = internal.value().children[child_index].page_id;
        cur.offset = internal.value().children[child_index].offset;
    }
    return base::Result<PageRef>::failure(
        error(base::ErrorCode::kCorruptData, "file_backup.index_depth_limit"));
}

[[nodiscard]] index::IndexKey parent_lower_bound_key(const std::uint64_t parent_entry_id) {
    index::IndexKey key;
    key.parent_entry_id = parent_entry_id;
    key.name_encoding = index::kNameEncodingWindowsUtf16Le;
    key.entry_id = 0;
    return key;
}

[[nodiscard]] base::Result<const std::vector<contracts::FileEntryDesc>*>
load_namespace_leaf(std::ifstream& input, OpenedFileArchive& state, const PageRef& leaf) {
    if (state.leaf_cache_offset == leaf.offset && !state.leaf_cache_entries.empty()) {
        return base::Result<const std::vector<contracts::FileEntryDesc>*>::success(
            &state.leaf_cache_entries);
    }
    auto page = get_page_cached(input, state, leaf.offset, leaf.page_id);
    if (!page) {
        return base::Result<const std::vector<contracts::FileEntryDesc>*>::failure(page.error());
    }
    if (page.value().header.page_kind != archive::kIndexPageLeaf) {
        return base::Result<const std::vector<contracts::FileEntryDesc>*>::failure(
            error(base::ErrorCode::kCorruptData, "file index leaf page kind is invalid"));
    }
    auto decoded = index::decode_leaf_page_cbor(page.value().plaintext);
    if (!decoded) {
        return base::Result<const std::vector<contracts::FileEntryDesc>*>::failure(decoded.error());
    }
    state.leaf_cache_offset = leaf.offset;
    state.leaf_cache_entries = std::move(decoded).value().entries;
    return base::Result<const std::vector<contracts::FileEntryDesc>*>::success(
        &state.leaf_cache_entries);
}

[[nodiscard]] base::Result<index::EntryIdIndexRecord>
lookup_entry_id_record(std::ifstream& input, OpenedFileArchive& state,
                       const std::uint64_t entry_id) {
    if (entry_id == 0) {
        return base::Result<index::EntryIdIndexRecord>::failure(
            error(base::ErrorCode::kNotFound, "file entry was not found"));
    }
    PageRef root{state.footer.entry_id_root.page_id, state.footer.entry_id_root.offset};
    auto leaf = navigate_integer(input, state, root, entry_id, index::kPageKindEntryIdLeaf,
                                 index::kPageKindEntryIdInternal);
    if (!leaf) {
        return base::Result<index::EntryIdIndexRecord>::failure(leaf.error());
    }
    auto page = get_page_cached(input, state, leaf.value().offset, leaf.value().page_id);
    if (!page) {
        return base::Result<index::EntryIdIndexRecord>::failure(page.error());
    }
    auto body = index::decode_entry_id_leaf_page_cbor(page.value().plaintext);
    if (!body) {
        return base::Result<index::EntryIdIndexRecord>::failure(body.error());
    }
    for (const auto& rec : body.value().records) {
        if (rec.entry_id == entry_id) {
            return base::Result<index::EntryIdIndexRecord>::success(rec);
        }
        if (rec.entry_id > entry_id) {
            break;
        }
    }
    return base::Result<index::EntryIdIndexRecord>::failure(
        error(base::ErrorCode::kNotFound, "file entry was not found"));
}

[[nodiscard]] base::Result<bool>
entry_has_children(std::ifstream& input, OpenedFileArchive& state, const std::uint64_t entry_id) {
    auto cursor = seek_namespace(input, state, parent_lower_bound_key(entry_id));
    if (!cursor) {
        return base::Result<bool>::failure(cursor.error());
    }
    for (;;) {
        auto leaf = load_namespace_leaf(input, state, cursor.value().leaf);
        if (!leaf) {
            return base::Result<bool>::failure(leaf.error());
        }
        for (const auto& entry : *leaf.value()) {
            if (entry.parent_entry_id < entry_id) {
                continue;
            }
            if (entry.parent_entry_id > entry_id) {
                return base::Result<bool>::success(false);
            }
            return base::Result<bool>::success(true);
        }
        auto advanced = advance_namespace(input, state, cursor.value());
        if (!advanced) {
            return base::Result<bool>::failure(advanced.error());
        }
        if (!advanced.value()) {
            return base::Result<bool>::success(false);
        }
    }
}

[[nodiscard]] base::Result<contracts::RecoveryPointEntrySummary>
make_summary(std::ifstream& input, OpenedFileArchive& state, const contracts::FileEntryDesc& entry) {
    contracts::RecoveryPointEntrySummary summary;
    summary.entry_id = std::to_string(entry.entry_id);
    summary.entry_kind = entry.kind;
    summary.logical_size_bytes = entry.logical_size;
    auto children = entry_has_children(input, state, entry.entry_id);
    if (!children) {
        return base::Result<contracts::RecoveryPointEntrySummary>::failure(children.error());
    }
    summary.has_children = children.value();
    return base::Result<contracts::RecoveryPointEntrySummary>::success(std::move(summary));
}

[[nodiscard]] base::Result<void>
install_file_ciphers(OpenedFileArchive& state, const std::string_view password) {
    if (!state.encryption_enabled) {
        return base::Result<void>::success();
    }
    if (password.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kUnauthorized, "shell.password_required"));
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
    if (footer.value().deduplicated_block_count != 0 ||
        footer.value().deduplicated_logical_bytes != 0) {
        return base::Result<archive::BackupFooter>::failure(
            error(base::ErrorCode::kCorruptData,
                  "file archive footer contains volume dedup metrics"));
    }
    return base::Result<archive::BackupFooter>::success(std::move(footer).value());
}

// --- compact parent graph for Verify ---
struct CompactEntry final {
    std::uint64_t entry_id{0};
    std::uint64_t parent_entry_id{0};
    std::uint8_t kind{0};
};

[[nodiscard]] base::Result<std::size_t>
find_entry_index(const std::vector<CompactEntry>& entries, const std::uint64_t entry_id) {
    const auto it = std::lower_bound(
        entries.begin(), entries.end(), entry_id,
        [](const CompactEntry& rec, const std::uint64_t value) { return rec.entry_id < value; });
    if (it == entries.end() || it->entry_id != entry_id) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
    }
    return base::Result<std::size_t>::success(static_cast<std::size_t>(it - entries.begin()));
}

[[nodiscard]] base::Result<void>
paint_parent_chain(const std::vector<CompactEntry>& entries, const std::size_t start,
                   std::vector<std::uint8_t>& colors, std::vector<std::uint32_t>& depths) {
    constexpr std::uint8_t kGray = 1;
    constexpr std::uint8_t kBlack = 2;
    std::vector<std::size_t> path;
    auto current = start;
    std::uint32_t base_depth = 0;
    for (;;) {
        if (colors[current] == kGray) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        if (colors[current] == kBlack) {
            base_depth = depths[current];
            break;
        }
        colors[current] = kGray;
        path.push_back(current);
        if (path.size() > contracts::kMaximumFileDirectoryDepth) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        const auto parent_id = entries[current].parent_entry_id;
        if (parent_id == 0) {
            break;
        }
        auto parent = find_entry_index(entries, parent_id);
        if (!parent) {
            return base::Result<void>::failure(parent.error());
        }
        current = parent.value();
    }
    while (!path.empty()) {
        if (base_depth >= contracts::kMaximumFileDirectoryDepth) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        ++base_depth;
        const auto index = path.back();
        path.pop_back();
        depths[index] = base_depth;
        colors[index] = kBlack;
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
collect_entry_id_leaf_records(std::ifstream& input, OpenedFileArchive& state, const PageRef& node,
                              const std::uint32_t depth, std::vector<CompactEntry>& out) {
    if (depth > index::kMaximumIndexDepth) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file_backup.index_depth_limit"));
    }
    auto page = get_page_cached(input, state, node.offset, node.page_id);
    if (!page) {
        return base::Result<void>::failure(page.error());
    }
    if (page.value().header.page_kind == index::kPageKindEntryIdLeaf) {
        auto body = index::decode_entry_id_leaf_page_cbor(page.value().plaintext);
        if (!body) {
            return base::Result<void>::failure(body.error());
        }
        for (const auto& rec : body.value().records) {
            CompactEntry compact;
            compact.entry_id = rec.entry_id;
            compact.parent_entry_id = rec.parent_entry_id;
            compact.kind = rec.kind;
            out.push_back(compact);
        }
        return base::Result<void>::success();
    }
    if (page.value().header.page_kind != index::kPageKindEntryIdInternal) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "entry id index page kind is invalid"));
    }
    auto internal = index::decode_integer_internal_page_cbor(page.value().plaintext);
    if (!internal) {
        return base::Result<void>::failure(internal.error());
    }
    for (const auto& child : internal.value().children) {
        auto walked =
            collect_entry_id_leaf_records(input, state, PageRef{child.page_id, child.offset},
                                          depth + 1, out);
        if (!walked) {
            return walked;
        }
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<std::vector<std::byte>>
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

base::Result<std::uint64_t> read_stream_size(std::ifstream& input) {
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

std::string digest_to_hex(const std::array<std::byte, 32>& digest) {
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

base::Result<std::vector<std::byte>>
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

base::Result<void> prepare_roots(std::ifstream& input, OpenedFileArchive& state) {
    const auto& footer = state.footer;
    if (footer.index_page_count == 0 || footer.index_root_offset == 0 ||
        footer.index_root_page_id == 0 || footer.entry_count == 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index root is missing"));
    }
    archive::IndexRootLocator namespace_root;
    namespace_root.page_id = footer.index_root_page_id;
    namespace_root.offset = footer.index_root_offset;
    namespace_root.digest = footer.index_root_digest;
    auto ns = verify_root_page(input, state, namespace_root, index::kIndexRootDigestLabel,
                               footer.entry_count, footer.stream_count, true);
    if (!ns) {
        return ns;
    }
    auto entry = verify_root_page(input, state, footer.entry_id_root,
                                  index::kEntryIdIndexRootDigestLabel, footer.entry_count, 0, true);
    if (!entry) {
        return entry;
    }
    auto stream = verify_root_page(input, state, footer.stream_root,
                                   index::kStreamIndexRootDigestLabel, footer.stream_count, 0,
                                   footer.stream_count > 0);
    if (!stream) {
        return stream;
    }
    auto chunk = verify_root_page(input, state, footer.chunk_root, index::kChunkIndexRootDigestLabel,
                                  footer.file_stream_chunk_count, 0,
                                  footer.file_stream_chunk_count > 0);
    if (!chunk) {
        return chunk;
    }
    state.roots_ready = true;
    return base::Result<void>::success();
}

base::Result<void> ensure_roots(std::ifstream& input, OpenedFileArchive& state) {
    if (state.roots_ready) {
        return base::Result<void>::success();
    }
    return prepare_roots(input, state);
}

base::Result<contracts::FileEntryDesc>
load_entry_by_id(std::ifstream& input, OpenedFileArchive& state, const std::uint64_t entry_id) {
    auto rec = lookup_entry_id_record(input, state, entry_id);
    if (!rec) {
        return base::Result<contracts::FileEntryDesc>::failure(rec.error());
    }
    PageRef leaf{rec.value().page_id, rec.value().page_offset};
    auto entries = load_namespace_leaf(input, state, leaf);
    if (!entries) {
        return base::Result<contracts::FileEntryDesc>::failure(entries.error());
    }
    if (rec.value().slot >= entries.value()->size()) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kCorruptData, "entry id index slot is invalid"));
    }
    const auto& entry = (*entries.value())[rec.value().slot];
    if (entry.entry_id != entry_id || entry.parent_entry_id != rec.value().parent_entry_id ||
        static_cast<std::uint8_t>(entry.kind) != rec.value().kind) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kCorruptData, "entry id index locator mismatch"));
    }
    return base::Result<contracts::FileEntryDesc>::success(entry);
}

base::Result<index::StreamIndexRecord>
lookup_stream_record(std::ifstream& input, OpenedFileArchive& state,
                     const std::uint32_t stream_index) {
    if (stream_index == 0 || state.footer.stream_root.page_id == 0) {
        return base::Result<index::StreamIndexRecord>::failure(
            error(base::ErrorCode::kNotFound, "file stream was not found"));
    }
    PageRef root{state.footer.stream_root.page_id, state.footer.stream_root.offset};
    auto leaf = navigate_integer(input, state, root, stream_index, index::kPageKindStreamLeaf,
                                 index::kPageKindStreamInternal);
    if (!leaf) {
        return base::Result<index::StreamIndexRecord>::failure(leaf.error());
    }
    auto page = get_page_cached(input, state, leaf.value().offset, leaf.value().page_id);
    if (!page) {
        return base::Result<index::StreamIndexRecord>::failure(page.error());
    }
    auto body = index::decode_stream_leaf_page_cbor(page.value().plaintext);
    if (!body) {
        return base::Result<index::StreamIndexRecord>::failure(body.error());
    }
    for (const auto& rec : body.value().records) {
        if (rec.stream_index == stream_index) {
            return base::Result<index::StreamIndexRecord>::success(rec);
        }
        if (rec.stream_index > stream_index) {
            break;
        }
    }
    return base::Result<index::StreamIndexRecord>::failure(
        error(base::ErrorCode::kNotFound, "file stream was not found"));
}

base::Result<StreamChunkLocator>
load_chunk_locator(std::ifstream& input, OpenedFileArchive& state, const std::uint64_t chunk_index) {
    if (state.footer.chunk_root.page_id == 0) {
        return base::Result<StreamChunkLocator>::failure(
            error(base::ErrorCode::kCorruptData, "stream extent references missing chunk"));
    }
    PageRef root{state.footer.chunk_root.page_id, state.footer.chunk_root.offset};
    auto leaf = navigate_integer(input, state, root, chunk_index, index::kPageKindChunkLeaf,
                                 index::kPageKindChunkInternal);
    if (!leaf) {
        return base::Result<StreamChunkLocator>::failure(leaf.error());
    }
    auto page = get_page_cached(input, state, leaf.value().offset, leaf.value().page_id);
    if (!page) {
        return base::Result<StreamChunkLocator>::failure(page.error());
    }
    auto body = index::decode_chunk_leaf_page_cbor(page.value().plaintext);
    if (!body) {
        return base::Result<StreamChunkLocator>::failure(body.error());
    }
    const index::ChunkIndexRecord* found = nullptr;
    for (const auto& rec : body.value().records) {
        if (rec.chunk_index == chunk_index) {
            found = &rec;
            break;
        }
        if (rec.chunk_index > chunk_index) {
            break;
        }
    }
    if (found == nullptr) {
        return base::Result<StreamChunkLocator>::failure(
            error(base::ErrorCode::kCorruptData, "stream extent references missing chunk"));
    }
    auto prefix_bytes = read_exact(input, found->record_offset, archive::kArchiveRecordPrefixSize);
    if (!prefix_bytes) {
        return base::Result<StreamChunkLocator>::failure(prefix_bytes.error());
    }
    auto prefix = archive::decode_archive_record_prefix(prefix_bytes.value());
    if (!prefix || prefix.value().record_kind != archive::kRecordKindFileStreamChunk) {
        return base::Result<StreamChunkLocator>::failure(
            error(base::ErrorCode::kCorruptData, "file stream chunk record is invalid"));
    }
    const auto header_offset = found->record_offset + archive::kArchiveRecordPrefixSize;
    auto header_bytes = read_exact(input, header_offset, archive::kFileStreamChunkHeaderSize);
    if (!header_bytes) {
        return base::Result<StreamChunkLocator>::failure(header_bytes.error());
    }
    auto header = archive::decode_file_stream_chunk_header(header_bytes.value());
    if (!header) {
        return base::Result<StreamChunkLocator>::failure(header.error());
    }
    if (header.value().chunk_index != chunk_index ||
        header.value().payload_size != found->payload_size ||
        header.value().block_entry_count != found->block_entry_count) {
        return base::Result<StreamChunkLocator>::failure(
            error(base::ErrorCode::kCorruptData, "chunk index record is inconsistent"));
    }
    StreamChunkLocator locator;
    locator.header = header.value();
    const auto entries_offset = header_offset + archive::kFileStreamChunkHeaderSize;
    const auto entries_bytes =
        static_cast<std::uint64_t>(found->block_entry_count) * archive::kBlockEntrySize;
    const auto expected_payload_offset = entries_offset + entries_bytes;
    if (found->payload_offset != expected_payload_offset ||
        prefix.value().body_size != entries_bytes + found->payload_size) {
        return base::Result<StreamChunkLocator>::failure(
            error(base::ErrorCode::kCorruptData, "chunk index record layout is inconsistent"));
    }
    locator.payload_offset = expected_payload_offset;
    locator.entries.reserve(found->block_entry_count);
    for (std::uint32_t i = 0; i < found->block_entry_count; ++i) {
        auto entry_bytes =
            read_exact(input, entries_offset + static_cast<std::uint64_t>(i) * archive::kBlockEntrySize,
                       archive::kBlockEntrySize);
        if (!entry_bytes) {
            return base::Result<StreamChunkLocator>::failure(entry_bytes.error());
        }
        auto entry = archive::decode_block_entry(entry_bytes.value());
        if (!entry) {
            return base::Result<StreamChunkLocator>::failure(entry.error());
        }
        locator.entries.push_back(entry.value());
    }
    return base::Result<StreamChunkLocator>::success(std::move(locator));
}

base::Result<ports::FileEntryPage>
list_children(std::ifstream& input, OpenedFileArchive& state, const std::uint64_t parent_entry_id,
              const std::uint32_t maximum_results, const std::uint64_t start_matched,
              const base::CancellationToken& cancellation) {
    ports::FileEntryPage page;
    std::uint64_t matched = 0;
    auto cursor = seek_namespace(input, state, parent_lower_bound_key(parent_entry_id));
    if (!cursor) {
        return base::Result<ports::FileEntryPage>::failure(cursor.error());
    }
    for (;;) {
        if (cancellation.stop_requested()) {
            return base::Result<ports::FileEntryPage>::failure(
                error(base::ErrorCode::kCancelled, "list cancelled"));
        }
        auto leaf = load_namespace_leaf(input, state, cursor.value().leaf);
        if (!leaf) {
            return base::Result<ports::FileEntryPage>::failure(leaf.error());
        }
        bool past_parent = false;
        for (const auto& entry : *leaf.value()) {
            if (entry.parent_entry_id < parent_entry_id) {
                continue;
            }
            if (entry.parent_entry_id > parent_entry_id) {
                past_parent = true;
                break;
            }
            if (matched < start_matched) {
                ++matched;
                continue;
            }
            if (page.items.size() >= maximum_results) {
                page.continuation_token = std::to_string(matched);
                return base::Result<ports::FileEntryPage>::success(std::move(page));
            }
            auto summary = make_summary(input, state, entry);
            if (!summary) {
                return base::Result<ports::FileEntryPage>::failure(summary.error());
            }
            page.items.push_back(std::move(summary).value());
            ++matched;
        }
        if (past_parent) {
            return base::Result<ports::FileEntryPage>::success(std::move(page));
        }
        auto advanced = advance_namespace(input, state, cursor.value());
        if (!advanced) {
            return base::Result<ports::FileEntryPage>::failure(advanced.error());
        }
        if (!advanced.value()) {
            return base::Result<ports::FileEntryPage>::success(std::move(page));
        }
    }
}

base::Result<void> for_each_entry_in_leaf_order(
    std::ifstream& input, OpenedFileArchive& state, const base::CancellationToken& cancellation,
    const std::function<base::Result<void>(const contracts::FileEntryDesc&)>& visitor) {
    index::IndexKey cursor;
    cursor.parent_entry_id = 0;
    cursor.name_encoding = index::kNameEncodingWindowsUtf16Le;
    cursor.entry_id = 0;
    auto cursor_result = seek_namespace(input, state, cursor);
    if (!cursor_result) {
        return base::Result<void>::failure(cursor_result.error());
    }
    auto tree_cursor = std::move(cursor_result).value();
    for (;;) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCancelled, "file archive entry walk cancelled"));
        }
        auto leaf = load_namespace_leaf(input, state, tree_cursor.leaf);
        if (!leaf) {
            return base::Result<void>::failure(leaf.error());
        }
        for (const auto& entry : *leaf.value()) {
            auto visited = visitor(entry);
            if (!visited) {
                return visited;
            }
        }
        auto advanced = advance_namespace(input, state, tree_cursor);
        if (!advanced) {
            return base::Result<void>::failure(advanced.error());
        }
        if (!advanced.value()) {
            return base::Result<void>::success();
        }
    }
}

base::Result<void>
verify_entry_id_index_and_parent_graph(std::ifstream& input, OpenedFileArchive& state,
                                       const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCancelled, "file archive verify cancelled"));
    }
    auto pages = verify_index_page_count(input, state);
    if (!pages) {
        return pages;
    }
    std::vector<CompactEntry> entries;
    entries.reserve(static_cast<std::size_t>(
        (std::min)(state.footer.entry_count, static_cast<std::uint64_t>(10'000'000))));
    PageRef root{state.footer.entry_id_root.page_id, state.footer.entry_id_root.offset};
    auto collected = collect_entry_id_leaf_records(input, state, root, 0, entries);
    if (!collected) {
        return collected;
    }
    if (entries.size() != state.footer.entry_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index entry count mismatch"));
    }
    for (std::size_t i = 0; i < entries.size(); ++i) {
        if (entries[i].entry_id == 0 || entries[i].parent_entry_id == entries[i].entry_id) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        if (i > 0 && entries[i].entry_id <= entries[i - 1].entry_id) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        if (entries[i].parent_entry_id == 0) {
            continue;
        }
        const auto it = std::lower_bound(
            entries.begin(), entries.end(), entries[i].parent_entry_id,
            [](const CompactEntry& rec, const std::uint64_t value) { return rec.entry_id < value; });
        if (it == entries.end() || it->entry_id != entries[i].parent_entry_id ||
            it->kind != static_cast<std::uint8_t>(contracts::FileEntryKind::kDirectory)) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
    }
    std::vector<bool> namespace_seen(entries.size(), false);
    std::uint64_t namespace_count = 0;
    auto namespace_valid = for_each_entry_in_leaf_order(
        input, state, cancellation, [&](const contracts::FileEntryDesc& entry) {
            auto found = find_entry_index(entries, entry.entry_id);
            if (!found || namespace_seen[found.value()]) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
            }
            const auto& compact = entries[found.value()];
            if (compact.parent_entry_id != entry.parent_entry_id ||
                compact.kind != static_cast<std::uint8_t>(entry.kind)) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kCorruptData, "format.corrupt_index"));
            }
            namespace_seen[found.value()] = true;
            ++namespace_count;
            return base::Result<void>::success();
        });
    if (!namespace_valid) {
        return namespace_valid;
    }
    if (namespace_count != state.footer.entry_count) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file index entry count mismatch"));
    }
    std::vector<std::uint8_t> colors(entries.size(), 0);
    std::vector<std::uint32_t> depths(entries.size(), 0);
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCancelled, "file archive verify cancelled"));
        }
        auto painted = paint_parent_chain(entries, index, colors, depths);
        if (!painted) {
            return painted;
        }
    }
    return base::Result<void>::success();
}

base::Result<OpenedFileArchive>
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
    if (request.index_load == FileArchiveIndexLoad::kDeferred) {
        return base::Result<OpenedFileArchive>::success(std::move(state));
    }
    auto roots = prepare_roots(input, state);
    if (!roots) {
        return base::Result<OpenedFileArchive>::failure(roots.error());
    }
    return base::Result<OpenedFileArchive>::success(std::move(state));
}

base::Result<std::uint64_t> parse_token(const std::optional<std::string>& token) {
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

} // namespace aegra::adapters::personal_archive::lazy_index
