#include "personal_archive_chunk_builder.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"

#include <algorithm>
#include <limits>
#include <string>
#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

namespace archive = format::personal_archive;

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] bool is_zero_block(const std::span<const std::byte> block) {
    return std::all_of(block.begin(), block.end(),
                       [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] base::Result<archive::SidecarRecord>
describe_block(const std::span<const std::byte> block) {
    if (is_zero_block(block)) {
        return base::Result<archive::SidecarRecord>::success(
            {archive::SidecarBlockState::kZero, {}});
    }
    auto digest = crypto_sodium::sha256(block);
    if (!digest) {
        return base::Result<archive::SidecarRecord>::failure(digest.error());
    }
    return base::Result<archive::SidecarRecord>::success(
        {archive::SidecarBlockState::kData, digest.value()});
}

[[nodiscard]] bool records_match(const archive::SidecarRecord& current,
                                 const archive::SidecarRecord& baseline) noexcept {
    if (current.state == archive::SidecarBlockState::kData) {
        return baseline.state == archive::SidecarBlockState::kData && current.hash == baseline.hash;
    }
    return baseline.state != archive::SidecarBlockState::kData;
}

void append_zero_block(PreparedArchiveChunk& chunk, const std::uint64_t block_index) {
    if (!chunk.entries.empty()) {
        auto& previous = chunk.entries.back();
        if (previous.flags == archive::kBlockFlagZero &&
            previous.logical_block_index + previous.logical_size == block_index &&
            previous.logical_size < (std::numeric_limits<std::uint32_t>::max)()) {
            ++previous.logical_size;
            return;
        }
    }
    archive::BlockEntry entry;
    entry.logical_block_index = block_index;
    entry.flags = archive::kBlockFlagZero;
    entry.logical_size = 1;
    chunk.entries.push_back(entry);
}

[[nodiscard]] base::Result<void> append_data_block(PreparedArchiveChunk& chunk,
                                                   const std::span<const std::byte> block,
                                                   const std::uint64_t block_index) {
    auto compressed = compression_zstd::compress(block);
    if (!compressed) {
        return base::Result<void>::failure(compressed.error());
    }
    const bool use_compressed = compressed.value().size() < block.size();
    const auto stored = use_compressed ? std::span<const std::byte>(compressed.value()) : block;
    if (stored.size() > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<void>::failure(invalid("archive block exceeds format limit"));
    }
    archive::BlockEntry entry;
    entry.logical_block_index = block_index;
    entry.data_offset_or_reference = chunk.payload.size();
    entry.stored_size = static_cast<std::uint32_t>(stored.size());
    entry.logical_size = static_cast<std::uint32_t>(use_compressed ? stored.size() : block.size());
    entry.flags = use_compressed ? archive::kBlockFlagCompressed : archive::kBlockFlagRaw;
    chunk.entries.push_back(entry);
    chunk.payload.insert(chunk.payload.end(), stored.begin(), stored.end());
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> append_changed_block(PreparedArchiveChunk& chunk,
                                                      const std::span<const std::byte> block,
                                                      const archive::SidecarRecord& record,
                                                      const std::uint64_t block_index) {
    if (record.state == archive::SidecarBlockState::kZero) {
        append_zero_block(chunk, block_index);
        return base::Result<void>::success();
    }
    return append_data_block(chunk, block, block_index);
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

[[nodiscard]] bool block_changed(const ChunkPreparationRequest& request,
                                 const archive::SidecarRecord& current,
                                 const std::uint64_t block_index) {
    return !request.incremental ||
           !records_match(current, request.baseline[static_cast<std::size_t>(block_index)]);
}

[[nodiscard]] base::Result<void> process_block(PreparedArchiveInput& result,
                                               PreparedArchiveChunk& current,
                                               const ChunkPreparationRequest& request,
                                               const std::span<const std::byte> block,
                                               const std::uint64_t block_index) {
    auto record = describe_block(block);
    if (!record) {
        return base::Result<void>::failure(record.error());
    }
    if (request.incremental && block_index >= request.baseline.size()) {
        return base::Result<void>::failure(
            invalid("incremental baseline does not cover the source"));
    }
    result.sidecar_records.push_back(record.value());
    if (!block_changed(request, record.value(), block_index)) {
        finish_chunk(result, current, request);
        return base::Result<void>::success();
    }
    return append_changed_block(current, block, record.value(), block_index);
}

} // namespace

base::Result<PreparedArchiveInput> prepare_archive_chunks(const ChunkPreparationRequest& request) {
    PreparedArchiveInput result;
    PreparedArchiveChunk current;
    std::size_t offset = 0;
    while (offset < request.input.payload.size()) {
        const auto remaining = request.input.payload.size() - offset;
        const auto block_size = (std::min)(remaining, static_cast<std::size_t>(request.block_size));
        const auto logical_offset = request.input.descriptor.logical_offset + offset;
        const auto block_index = logical_offset / request.block_size;
        auto processed =
            process_block(result, current, request,
                          request.input.payload.subspan(offset, block_size), block_index);
        if (!processed) {
            return base::Result<PreparedArchiveInput>::failure(processed.error());
        }
        offset += block_size;
    }
    finish_chunk(result, current, request);
    return base::Result<PreparedArchiveInput>::success(std::move(result));
}

} // namespace aegra::adapters::personal_archive::detail
