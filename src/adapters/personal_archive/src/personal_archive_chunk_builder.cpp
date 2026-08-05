#include "personal_archive_chunk_builder.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/adapters/crypto_sodium/content_hash.h"
#include "aegra/base/error.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive::detail {
namespace {

namespace archive = format::personal_archive;

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

// Word-oriented zero check (old engine treated free blocks as known-zero without hashing).
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
    base::Error error{};
    bool failed{false};
};

[[nodiscard]] PreparedBlock prepare_one_block(const ChunkPreparationRequest& request,
                                              const std::span<const std::byte> block,
                                              const std::uint64_t block_index) {
    PreparedBlock out;
    out.block_index = block_index;
    if (request.incremental && block_index >= request.baseline.size()) {
        out.failed = true;
        out.error = invalid("incremental baseline does not cover the source");
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
    auto compressed = compression_zstd::compress(block);
    if (!compressed) {
        out.failed = true;
        out.error = compressed.error();
        return out;
    }
    if (compressed.value().size() < block.size()) {
        out.use_compressed = true;
        out.stored = std::move(compressed).value();
    } else {
        out.stored.assign(block.begin(), block.end());
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
    entry.logical_size = static_cast<std::uint32_t>(block.stored.size());
    entry.flags =
        block.use_compressed ? archive::kBlockFlagCompressed : archive::kBlockFlagRaw;
    chunk.entries.push_back(entry);
    chunk.payload.insert(chunk.payload.end(), block.stored.begin(), block.stored.end());
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

[[nodiscard]] std::uint32_t worker_count_for(const std::size_t block_count) noexcept {
    const auto hardware = std::thread::hardware_concurrency();
    const auto available = hardware == 0 ? 4U : hardware;
    const auto by_blocks = static_cast<std::uint32_t>((std::max)(block_count, std::size_t{1}));
    return (std::max)(1U, (std::min)(available, by_blocks));
}

[[nodiscard]] base::Result<std::vector<PreparedBlock>>
prepare_blocks_parallel(const ChunkPreparationRequest& request) {
    const auto& payload = request.input.payload;
    const auto block_size = static_cast<std::size_t>(request.block_size);
    const auto block_count = (payload.size() + block_size - 1U) / block_size;
    std::vector<PreparedBlock> blocks(block_count);
    const auto workers = worker_count_for(block_count);
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    base::Error first_error{};
    std::mutex error_mutex;
    std::vector<std::thread> threads;
    threads.reserve(workers);
    const auto record_failure = [&](base::Error error) {
        const std::scoped_lock lock(error_mutex);
        if (!failed.exchange(true, std::memory_order_relaxed)) {
            first_error = std::move(error);
        }
    };
    try {
        for (std::uint32_t worker = 0; worker < workers; ++worker) {
            threads.emplace_back([&] {
                try {
                    for (;;) {
                        if (failed.load(std::memory_order_relaxed)) {
                            return;
                        }
                        const auto index = next.fetch_add(1, std::memory_order_relaxed);
                        if (index >= block_count) {
                            return;
                        }
                        const auto offset = index * block_size;
                        const auto size = (std::min)(block_size, payload.size() - offset);
                        const auto logical_offset =
                            request.input.descriptor.logical_offset + offset;
                        const auto block_index = logical_offset / request.block_size;
                        blocks[index] = prepare_one_block(
                            request, payload.subspan(offset, size), block_index);
                        if (blocks[index].failed) {
                            record_failure(blocks[index].error);
                            return;
                        }
                    }
                } catch (...) {
                    record_failure(base::Error{base::ErrorCode::kInternal,
                                               "archive block worker failed unexpectedly"});
                }
            });
        }
    } catch (...) {
        record_failure(
            base::Error{base::ErrorCode::kInternal, "failed to start archive block workers"});
    }
    for (auto& thread : threads) {
        if (thread.joinable()) {
            thread.join();
        }
    }
    if (failed.load(std::memory_order_relaxed)) {
        return base::Result<std::vector<PreparedBlock>>::failure(first_error);
    }
    return base::Result<std::vector<PreparedBlock>>::success(std::move(blocks));
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
        append_prepared_data(current, block);
        if (block.failed) {
            return base::Result<PreparedArchiveInput>::failure(block.error);
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

} // namespace

base::Result<PreparedArchiveInput> prepare_archive_chunks(const ChunkPreparationRequest& request) {
    if (request.block_size == 0 || request.input.payload.empty()) {
        return base::Result<PreparedArchiveInput>::success({});
    }
    // Free-skip / sparse zeros: one scan then run-length ZERO (old free blocks never hashed).
    if (is_zero_block(request.input.payload)) {
        return prepare_all_zero_payload(request);
    }
    // Parallel hash + compress per block (old BackupEngine workerCount = hardware_concurrency).
    auto blocks = prepare_blocks_parallel(request);
    if (!blocks) {
        return base::Result<PreparedArchiveInput>::failure(blocks.error());
    }
    return assemble_prepared_blocks(request, std::move(blocks).value());
}

} // namespace aegra::adapters::personal_archive::detail
