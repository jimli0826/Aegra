#include "personal_archive_chunk_builder.h"

#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive::detail {
namespace {

namespace archive = format::personal_archive;

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

// Word-oriented zero check for allocated blocks. FREE blocks are classified before this scan.
[[nodiscard]] bool is_zero_block(const std::span<const std::byte> block) noexcept {
    if (block.empty()) {
        return true;
    }
    std::size_t index = 0;
    // Align head.
    while (index < block.size() &&
           (reinterpret_cast<std::uintptr_t>(block.data() + index) % sizeof(std::uint64_t)) != 0) {
        if (block[index] != std::byte{0}) {
            return false;
        }
        ++index;
    }
    for (; index + sizeof(std::uint64_t) <= block.size(); index += sizeof(std::uint64_t)) {
        std::uint64_t word = 0;
        std::memcpy(&word, block.data() + index, sizeof(word));
        if (word != 0) {
            return false;
        }
    }
    for (; index < block.size(); ++index) {
        if (block[index] != std::byte{0}) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] bool records_match(const archive::SidecarRecord& current,
                                 const archive::SidecarRecord& baseline) noexcept {
    if (current.state == archive::SidecarBlockState::kData) {
        return baseline.state == archive::SidecarBlockState::kData && current.hash == baseline.hash;
    }
    return baseline.state == current.state;
}

void append_state_block(PreparedArchiveChunk& chunk, const std::uint64_t block_index,
                        const std::uint8_t flag) {
    if (!chunk.entries.empty()) {
        auto& previous = chunk.entries.back();
        if (previous.flags == flag &&
            previous.logical_block_index + previous.logical_size == block_index &&
            previous.logical_size < (std::numeric_limits<std::uint32_t>::max)()) {
            ++previous.logical_size;
            return;
        }
    }
    archive::BlockEntry entry;
    entry.logical_block_index = block_index;
    entry.flags = flag;
    entry.logical_size = 1;
    chunk.entries.push_back(entry);
}

void append_zero_block(PreparedArchiveChunk& chunk, const std::uint64_t block_index) {
    append_state_block(chunk, block_index, archive::kBlockFlagZero);
}

void append_free_block(PreparedArchiveChunk& chunk, const std::uint64_t block_index) {
    append_state_block(chunk, block_index, archive::kBlockFlagFree);
}

[[nodiscard]] bool block_is_free(const ports::ChunkDescriptor& descriptor,
                                 const std::size_t source_offset,
                                 const std::size_t block_size) noexcept {
    const auto offset = static_cast<std::uint64_t>(source_offset);
    const auto size = static_cast<std::uint64_t>(block_size);
    const auto match = std::ranges::upper_bound(
        descriptor.free_ranges, offset, {},
        [](const ports::ChunkFreeRange& range) { return range.offset + range.size; });
    return match != descriptor.free_ranges.end() && match->offset <= offset &&
           size <= match->size - (offset - match->offset);
}

[[nodiscard]] bool block_changed(const ChunkPreparationRequest& request,
                                 const archive::SidecarRecord& current,
                                 const std::uint64_t block_index) {
    return !request.incremental ||
           !records_match(current, request.baseline[static_cast<std::size_t>(block_index)]);
}

struct PreparedBlock final {
    archive::SidecarRecord record{};
    std::vector<std::byte> stored;
    bool changed{true};
    bool use_compressed{false};
    std::uint64_t block_index{0};
    std::size_t source_offset{0};
    std::uint32_t plaintext_size{0};
    /// When dedup is on: emit DEDUP instead of storing payload.
    bool emit_dedup{false};
    std::uint32_t dedup_ref{0};
    /// When dedup is on: this block is a canonical that still needs zstd.
    bool needs_compress{false};
    base::Error error{};
    bool failed{false};
};

[[nodiscard]] base::Result<void>
compress_changed_data(PreparedBlock& block, const std::span<const std::byte> plaintext,
                      compression_zstd::ZstdCompressor& compressor) {
    auto compressed = compressor.compress(plaintext);
    if (!compressed) {
        return base::Result<void>::failure(compressed.error());
    }
    if (compressed.value().size() < plaintext.size()) {
        block.use_compressed = true;
        block.stored = std::move(compressed).value();
    } else {
        block.use_compressed = false;
        block.stored.assign(plaintext.begin(), plaintext.end());
    }
    return base::Result<void>::success();
}

[[nodiscard]] PreparedBlock prepare_one_block(const ChunkPreparationRequest& request,
                                              const std::span<const std::byte> block,
                                              const std::uint64_t block_index,
                                              const std::size_t source_offset,
                                              BlockWorkerLocal& local) {
    PreparedBlock out;
    out.block_index = block_index;
    out.source_offset = source_offset;
    if (block.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        out.failed = true;
        out.error = invalid("archive block exceeds format limit");
        return out;
    }
    out.plaintext_size = static_cast<std::uint32_t>(block.size());
    if (request.incremental && block_index >= request.baseline.size()) {
        out.failed = true;
        out.error = invalid("incremental baseline does not cover the source");
        return out;
    }
    if (block_is_free(request.input.descriptor, source_offset, block.size())) {
        out.record = {archive::SidecarBlockState::kFree, {}};
        out.changed = block_changed(request, out.record, block_index);
        return out;
    }
    if (is_zero_block(block)) {
        out.record = {archive::SidecarBlockState::kZero, {}};
        out.changed = block_changed(request, out.record, block_index);
        return out;
    }
    auto digest = crypto_sodium::sha256(block);
    if (!digest) {
        out.failed = true;
        out.error = digest.error();
        return out;
    }
    out.record = {archive::SidecarBlockState::kData, digest.value()};
    out.changed = block_changed(request, out.record, block_index);
    if (!out.changed) {
        return out;
    }
    // DEDUP: compress after sequential canonical selection (parallel phase below).
    if (request.deduplication_enabled) {
        return out;
    }
    auto compressed = compress_changed_data(out, block, local.compressor);
    if (!compressed) {
        out.failed = true;
        out.error = compressed.error();
    }
    return out;
}

void append_prepared_data(PreparedArchiveChunk& chunk, PreparedBlock& block) {
    if (block.stored.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        block.failed = true;
        block.error = invalid("archive block exceeds format limit");
        return;
    }
    archive::BlockEntry entry;
    entry.logical_block_index = block.block_index;
    entry.data_offset_or_reference = chunk.payload.size();
    entry.stored_size = static_cast<std::uint32_t>(block.stored.size());
    // Existing volume convention: logical_size equals stored_size for RAW/COMPRESSED.
    entry.logical_size = static_cast<std::uint32_t>(block.stored.size());
    entry.flags =
        block.use_compressed ? archive::kBlockFlagCompressed : archive::kBlockFlagRaw;
    chunk.entries.push_back(entry);
    chunk.payload.insert(chunk.payload.end(), block.stored.begin(), block.stored.end());
}

void append_dedup_block(PreparedArchiveChunk& chunk, const std::uint64_t block_index,
                        const std::uint64_t ref_index) {
    archive::BlockEntry entry;
    entry.logical_block_index = block_index;
    entry.data_offset_or_reference = ref_index;
    entry.stored_size = 0;
    entry.logical_size = 0;
    entry.flags = archive::kBlockFlagDedup;
    chunk.entries.push_back(entry);
}

// Placeholder RAW entry so DEDUP ref_index matches final emit order (payload filled later).
void append_store_placeholder(PreparedArchiveChunk& chunk, const std::uint64_t block_index) {
    archive::BlockEntry entry;
    entry.logical_block_index = block_index;
    entry.data_offset_or_reference = 0;
    entry.stored_size = 0;
    entry.logical_size = 0;
    entry.flags = archive::kBlockFlagRaw;
    chunk.entries.push_back(entry);
}

void finish_chunk(PreparedArchiveInput& result, PreparedArchiveChunk& chunk,
                  const ChunkPreparationRequest& request) {
    if (chunk.entries.empty()) {
        return;
    }
    chunk.header.chunk_index = request.first_archive_chunk_index + result.chunks.size();
    chunk.header.source_index = request.source_index;
    chunk.header.block_entry_count = static_cast<std::uint32_t>(chunk.entries.size());
    chunk.header.payload_size = chunk.payload.size();
    result.chunks.push_back(std::move(chunk));
    chunk = {};
}

// Per-physical-chunk DEDUP index (ADR-0022). Discarded on every finish_chunk.
struct DedupKey final {
    std::array<std::byte, 32> digest{};
    std::uint32_t logical_size{0};

    [[nodiscard]] friend bool operator==(const DedupKey&, const DedupKey&) = default;
};

struct DedupKeyHash final {
    [[nodiscard]] std::size_t operator()(const DedupKey& key) const noexcept {
        std::uint64_t word = 0;
        std::memcpy(&word, key.digest.data(), sizeof(word));
        return static_cast<std::size_t>(word ^ (static_cast<std::uint64_t>(key.logical_size) *
                                                0x9e3779b97f4a7c15ULL));
    }
};

struct DedupCanonical final {
    std::uint32_t entry_index{0};
    std::size_t source_offset{0};
    std::uint32_t logical_size{0};
};

struct DedupWindow final {
    std::unordered_map<DedupKey, std::vector<DedupCanonical>, DedupKeyHash> index;

    void clear() noexcept { index.clear(); }

    [[nodiscard]] std::optional<std::uint32_t>
    find_match(const DedupKey& key, const std::span<const std::byte> plaintext,
               const std::span<const std::byte> source_payload) const {
        const auto found = index.find(key);
        if (found == index.end()) {
            return std::nullopt;
        }
        for (const auto& candidate : found->second) {
            if (candidate.logical_size != plaintext.size() ||
                candidate.source_offset > source_payload.size() ||
                candidate.logical_size > source_payload.size() - candidate.source_offset) {
                continue;
            }
            const auto canonical =
                source_payload.subspan(candidate.source_offset, candidate.logical_size);
            if (std::equal(plaintext.begin(), plaintext.end(), canonical.begin())) {
                return candidate.entry_index;
            }
        }
        return std::nullopt;
    }

    void register_canonical(const DedupKey& key, const DedupCanonical& slot) {
        index[key].push_back(slot);
    }
};

[[nodiscard]] base::Result<void>
require_worker_pool(const ChunkPreparationRequest& request) {
    if (request.worker_pool == nullptr) {
        return base::Result<void>::failure(
            invalid("archive chunk preparation requires a session worker pool"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<PreparedBlock>>
prepare_blocks_parallel(const ChunkPreparationRequest& request) {
    auto pool_ok = require_worker_pool(request);
    if (!pool_ok) {
        return base::Result<std::vector<PreparedBlock>>::failure(pool_ok.error());
    }
    const auto& payload = request.input.payload;
    const auto block_size = static_cast<std::size_t>(request.block_size);
    const auto block_count = (payload.size() + block_size - 1U) / block_size;
    std::vector<PreparedBlock> blocks(block_count);
    auto ran = request.worker_pool->parallel_for(
        block_count, [&](const std::size_t index, BlockWorkerLocal& local) -> base::Result<void> {
            const auto offset = index * block_size;
            const auto size = (std::min)(block_size, payload.size() - offset);
            const auto logical_offset = request.input.descriptor.logical_offset + offset;
            const auto block_index = logical_offset / request.block_size;
            blocks[index] = prepare_one_block(request, payload.subspan(offset, size), block_index,
                                              offset, local);
            if (blocks[index].failed) {
                return base::Result<void>::failure(blocks[index].error);
            }
            return base::Result<void>::success();
        });
    if (!ran) {
        return base::Result<std::vector<PreparedBlock>>::failure(ran.error());
    }
    return base::Result<std::vector<PreparedBlock>>::success(std::move(blocks));
}

[[nodiscard]] base::Result<void>
account_dedup_metrics(PreparedArchiveInput& result, const std::uint32_t plaintext_size) {
    if (result.deduplicated_block_count == (std::numeric_limits<std::uint64_t>::max)() ||
        result.deduplicated_logical_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - plaintext_size) {
        return base::Result<void>::failure(invalid("dedup metrics overflow"));
    }
    ++result.deduplicated_block_count;
    result.deduplicated_logical_bytes += plaintext_size;
    return base::Result<void>::success();
}

// Sequential: choose first-canonical / DEDUP refs (ADR-0022). Simulates entry indexes only.
[[nodiscard]] base::Result<void> plan_dedup_selections(const ChunkPreparationRequest& request,
                                                       std::vector<PreparedBlock>& blocks,
                                                       PreparedArchiveInput& metrics) {
    DedupWindow window;
    PreparedArchiveChunk shape;
    for (auto& block : blocks) {
        if (block.failed) {
            return base::Result<void>::failure(block.error);
        }
        if (!block.changed) {
            window.clear();
            shape = {};
            continue;
        }
        if (block.record.state == archive::SidecarBlockState::kZero) {
            append_zero_block(shape, block.block_index);
            continue;
        }
        if (block.record.state == archive::SidecarBlockState::kFree) {
            append_free_block(shape, block.block_index);
            continue;
        }
        if (shape.entries.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
            return base::Result<void>::failure(invalid("archive chunk entry count exceeds limit"));
        }
        const auto plaintext =
            request.input.payload.subspan(block.source_offset, block.plaintext_size);
        DedupKey key;
        key.digest = block.record.hash;
        key.logical_size = block.plaintext_size;
        if (const auto match = window.find_match(key, plaintext, request.input.payload)) {
            block.emit_dedup = true;
            block.dedup_ref = *match;
            append_dedup_block(shape, block.block_index, *match);
            auto accounted = account_dedup_metrics(metrics, block.plaintext_size);
            if (!accounted) {
                return accounted;
            }
            continue;
        }
        block.needs_compress = true;
        const auto entry_index = static_cast<std::uint32_t>(shape.entries.size());
        append_store_placeholder(shape, block.block_index);
        window.register_canonical(
            key, DedupCanonical{entry_index, block.source_offset, block.plaintext_size});
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
compress_canonical_blocks_parallel(const ChunkPreparationRequest& request,
                                   std::vector<PreparedBlock>& blocks) {
    auto pool_ok = require_worker_pool(request);
    if (!pool_ok) {
        return pool_ok;
    }
    std::vector<std::size_t> pending;
    pending.reserve(blocks.size());
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (blocks[index].needs_compress) {
            pending.push_back(index);
        }
    }
    return request.worker_pool->parallel_for(
        pending.size(), [&](const std::size_t work, BlockWorkerLocal& local) -> base::Result<void> {
            auto& block = blocks[pending[work]];
            const auto plaintext =
                request.input.payload.subspan(block.source_offset, block.plaintext_size);
            auto compressed = compress_changed_data(block, plaintext, local.compressor);
            if (!compressed) {
                block.failed = true;
                block.error = compressed.error();
                return base::Result<void>::failure(compressed.error());
            }
            return base::Result<void>::success();
        });
}

[[nodiscard]] base::Result<void>
emit_changed_data_block(PreparedArchiveChunk& current, PreparedBlock& block,
                        const ChunkPreparationRequest& request) {
    if (request.deduplication_enabled && block.emit_dedup) {
        if (current.entries.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
            return base::Result<void>::failure(invalid("archive chunk entry count exceeds limit"));
        }
        append_dedup_block(current, block.block_index, block.dedup_ref);
        return base::Result<void>::success();
    }
    if (current.entries.size() >= (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<void>::failure(invalid("archive chunk entry count exceeds limit"));
    }
    append_prepared_data(current, block);
    return block.failed ? base::Result<void>::failure(block.error)
                        : base::Result<void>::success();
}

[[nodiscard]] base::Result<PreparedArchiveInput>
assemble_prepared_blocks(const ChunkPreparationRequest& request,
                         std::vector<PreparedBlock> blocks) {
    PreparedArchiveInput result;
    PreparedArchiveChunk current;
    result.sidecar_records.reserve(blocks.size());
    for (auto& block : blocks) {
        if (block.failed) {
            return base::Result<PreparedArchiveInput>::failure(block.error);
        }
        result.sidecar_records.push_back(block.record);
        if (!block.changed) {
            finish_chunk(result, current, request);
            continue;
        }
        if (block.record.state == archive::SidecarBlockState::kZero) {
            append_zero_block(current, block.block_index);
            continue;
        }
        if (block.record.state == archive::SidecarBlockState::kFree) {
            append_free_block(current, block.block_index);
            continue;
        }
        auto emitted = emit_changed_data_block(current, block, request);
        if (!emitted) {
            return base::Result<PreparedArchiveInput>::failure(emitted.error());
        }
    }
    finish_chunk(result, current, request);
    return base::Result<PreparedArchiveInput>::success(std::move(result));
}

[[nodiscard]] base::Result<PreparedArchiveInput>
prepare_all_zero_payload(const ChunkPreparationRequest& request) {
    PreparedArchiveInput result;
    PreparedArchiveChunk current;
    std::size_t offset = 0;
    while (offset < request.input.payload.size()) {
        const auto remaining = request.input.payload.size() - offset;
        const auto block_size = (std::min)(remaining, static_cast<std::size_t>(request.block_size));
        const auto logical_offset = request.input.descriptor.logical_offset + offset;
        const auto block_index = logical_offset / request.block_size;
        if (request.incremental && block_index >= request.baseline.size()) {
            return base::Result<PreparedArchiveInput>::failure(
                invalid("incremental baseline does not cover the source"));
        }
        const archive::SidecarRecord zero_record{archive::SidecarBlockState::kZero, {}};
        result.sidecar_records.push_back(zero_record);
        if (block_changed(request, zero_record, block_index)) {
            append_zero_block(current, block_index);
        } else {
            finish_chunk(result, current, request);
        }
        offset += block_size;
    }
    finish_chunk(result, current, request);
    return base::Result<PreparedArchiveInput>::success(std::move(result));
}

[[nodiscard]] base::Result<PreparedArchiveInput>
prepare_with_dedup(const ChunkPreparationRequest& request) {
    auto blocks = prepare_blocks_parallel(request);
    if (!blocks) {
        return base::Result<PreparedArchiveInput>::failure(blocks.error());
    }
    PreparedArchiveInput metrics;
    auto planned = plan_dedup_selections(request, blocks.value(), metrics);
    if (!planned) {
        return base::Result<PreparedArchiveInput>::failure(planned.error());
    }
    auto compressed = compress_canonical_blocks_parallel(request, blocks.value());
    if (!compressed) {
        return base::Result<PreparedArchiveInput>::failure(compressed.error());
    }
    auto assembled = assemble_prepared_blocks(request, std::move(blocks).value());
    if (!assembled) {
        return assembled;
    }
    assembled.value().deduplicated_block_count = metrics.deduplicated_block_count;
    assembled.value().deduplicated_logical_bytes = metrics.deduplicated_logical_bytes;
    return assembled;
}

} // namespace

base::Result<PreparedArchiveInput> prepare_archive_chunks(const ChunkPreparationRequest& request) {
    if (request.block_size == 0 || request.input.payload.empty()) {
        return base::Result<PreparedArchiveInput>::success({});
    }
    // Allocated all-zero input can use the fast path. FREE blocks require distinct records.
    if (request.input.descriptor.free_ranges.empty() && is_zero_block(request.input.payload)) {
        return prepare_all_zero_payload(request);
    }
    if (request.deduplication_enabled) {
        // Parallel hash -> sequential first-canonical/DEDUP plan -> parallel zstd -> emit.
        return prepare_with_dedup(request);
    }
    // Parallel hash + compress; sequential emit only.
    auto blocks = prepare_blocks_parallel(request);
    if (!blocks) {
        return base::Result<PreparedArchiveInput>::failure(blocks.error());
    }
    return assemble_prepared_blocks(request, std::move(blocks).value());
}

} // namespace aegra::adapters::personal_archive::detail
