#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_block_worker_pool.h"
#include "personal_archive_decoded_chunk_cache.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

struct OverlaySlice final {
    std::size_t layer_index{0};
    std::uint64_t source_chunk_index{0};
    std::uint64_t source_offset{0};
    std::uint64_t target_offset{0};
    std::uint64_t size{0};
};

struct ChainRecord final {
    ports::ChunkDescriptor descriptor;
    /// Visible overlay slices after pruning: pairwise disjoint in target space, base-first.
    std::vector<OverlaySlice> overlays;
    /// Sorted, disjoint target ranges the visible overlays rewrite (union of `overlays`).
    std::vector<ports::ChunkFreeRange> overlay_covered;
    /// The base layer holds no data for this chunk (every byte is FREE), so its payload is
    /// all zeros and need not be decoded before overlays are applied.
    bool base_all_free{false};
    /// The (pruned) overlays rewrite every byte, so the base payload is never visible.
    bool overlays_cover_chunk{false};

    [[nodiscard]] bool base_needed() const noexcept {
        return !base_all_free && !overlays_cover_chunk;
    }
};

/// Base chunk spans of one source volume in logical order, for range lookups.
struct RecordSpan final {
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
    std::size_t record_index{0};
};

struct VolumeRecords final {
    std::uint32_t volume_index{0};
    std::uint64_t volume_size{0};
    std::vector<RecordSpan> spans;
};

/// One contiguous run of a range read served by one archive chunk.
struct PieceRange final {
    detail::DecodedChunkKey key;
    /// Offset into the decoded payload of `key`.
    std::uint64_t payload_offset{0};
    /// Offset into the requested range.
    std::uint64_t range_offset{0};
    std::uint64_t size{0};
};

[[nodiscard]] std::uint64_t microseconds_since(const std::chrono::steady_clock::time_point since) {
    return static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() -
                                                              since)
            .count());
}

/// Atomic mirror of the chain part of ArchiveChainReadStatistics (reads may run on several
/// threads). Default ordering: diagnostics only, never control flow.
struct ChainCounters final {
    std::atomic<std::uint64_t> chunks_read{0};
    std::atomic<std::uint64_t> range_reads{0};
    std::atomic<std::uint64_t> range_bytes{0};
    std::atomic<std::uint64_t> piece_cache_hits{0};
    std::atomic<std::uint64_t> piece_load_waits{0};
    std::atomic<std::uint64_t> base_decodes{0};
    std::atomic<std::uint64_t> base_decode_microseconds{0};
    std::atomic<std::uint64_t> overlay_decodes{0};
    std::atomic<std::uint64_t> overlay_decode_microseconds{0};
    std::atomic<std::uint64_t> on_demand_decodes{0};
    std::atomic<std::uint64_t> on_demand_decode_microseconds{0};
    std::atomic<std::uint64_t> prefetch_decodes{0};
    std::atomic<std::uint64_t> prefetch_decode_microseconds{0};
    std::atomic<std::uint64_t> prefetch_failures{0};
};

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

// ---------------------------------------------------------------------------------------
// Chain construction: FREE bookkeeping, validation, overlay slicing and pruning.

void remove_free_range(std::vector<ports::ChunkFreeRange>& ranges, const std::uint64_t offset,
                       const std::uint64_t size) {
    const auto end = offset + size;
    std::vector<ports::ChunkFreeRange> result;
    result.reserve(ranges.size() + 1);
    for (const auto& range : ranges) {
        const auto range_end = range.offset + range.size;
        if (range_end <= offset || range.offset >= end) {
            result.push_back(range);
            continue;
        }
        if (range.offset < offset) {
            result.push_back({range.offset, offset - range.offset});
        }
        if (range_end > end) {
            result.push_back({end, range_end - end});
        }
    }
    ranges = std::move(result);
}

void add_free_range(std::vector<ports::ChunkFreeRange>& ranges, ports::ChunkFreeRange added) {
    ranges.push_back(added);
    std::ranges::sort(ranges, {}, &ports::ChunkFreeRange::offset);
    std::vector<ports::ChunkFreeRange> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (!merged.empty() && merged.back().offset + merged.back().size == range.offset) {
            merged.back().size += range.size;
        } else {
            merged.push_back(range);
        }
    }
    ranges = std::move(merged);
}

void apply_free_overlay(ChainRecord& record, const ports::ChunkDescriptor& overlay,
                        const OverlaySlice& slice) {
    remove_free_range(record.descriptor.free_ranges, slice.target_offset, slice.size);
    const auto source_end = slice.source_offset + slice.size;
    for (const auto& range : overlay.free_ranges) {
        const auto range_end = range.offset + range.size;
        const auto start = (std::max)(range.offset, slice.source_offset);
        const auto end = (std::min)(range_end, source_end);
        if (start < end) {
            add_free_range(record.descriptor.free_ranges,
                           {slice.target_offset + start - slice.source_offset, end - start});
        }
    }
}

/// Layer geometry must match the write-side incremental parent check: same ordered
/// volume_index / volume_id / total_size for every source volume (not single-volume only).
[[nodiscard]] bool same_volume_geometry(const format::Manifest& left,
                                        const format::Manifest& right) noexcept {
    if (left.volumes.size() != right.volumes.size() || left.volumes.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < left.volumes.size(); ++index) {
        const auto& left_volume = left.volumes[index];
        const auto& right_volume = right.volumes[index];
        if (left_volume.volume_index != right_volume.volume_index ||
            left_volume.volume_id != right_volume.volume_id ||
            left_volume.total_size != right_volume.total_size) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] base::Result<void> validate_layer(const PersonalArchiveReader& previous,
                                                const PersonalArchiveReader& current) {
    const auto& previous_identity = previous.identity();
    const auto& current_identity = current.identity();
    if (current_identity.backup_type != format::BackupType::kIncremental ||
        current_identity.parent_uuid != previous_identity.file_uuid ||
        current_identity.backup_set_uuid != previous_identity.backup_set_uuid) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain identity is invalid"));
    }
    if (current_identity.block_size != previous_identity.block_size ||
        !format::compatible_boot_profiles(previous.manifest().boot_profile,
                                          current.manifest().boot_profile) ||
        !same_volume_geometry(previous.manifest(), current.manifest())) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain source geometry changed"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_chain(const std::vector<std::unique_ptr<PersonalArchiveReader>>& layers) {
    if (layers.front()->identity().backup_type != format::BackupType::kFull) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain must begin with a full backup"));
    }
    std::vector<std::array<std::byte, 16>> identities;
    identities.reserve(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto& identity = layers[index]->identity().file_uuid;
        if (std::find(identities.begin(), identities.end(), identity) != identities.end()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kConflict, "archive chain contains a repeated UUID"));
        }
        identities.push_back(identity);
        if (index != 0) {
            auto valid = validate_layer(*layers[index - 1], *layers[index]);
            if (!valid) {
                return valid;
            }
        }
    }
    return base::Result<void>::success();
}

/// free_ranges are sorted and non-overlapping (ChunkDescriptor contract), so they cover
/// the chunk exactly when they tile [0, logical_size) without gaps.
[[nodiscard]] bool free_ranges_cover_chunk(const ports::ChunkDescriptor& descriptor) noexcept {
    std::uint64_t covered = 0;
    for (const auto& range : descriptor.free_ranges) {
        if (range.offset != covered) {
            return false;
        }
        covered += range.size;
    }
    return descriptor.logical_size > 0 && covered == descriptor.logical_size;
}

[[nodiscard]] base::Result<std::vector<ChainRecord>>
make_base_records(const PersonalArchiveReader& base) {
    std::vector<ChainRecord> result;
    result.reserve(static_cast<std::size_t>(base.chunk_count()));
    for (std::uint64_t index = 0; index < base.chunk_count(); ++index) {
        auto descriptor = base.describe_chunk(index);
        if (!descriptor) {
            return base::Result<std::vector<ChainRecord>>::failure(descriptor.error());
        }
        ChainRecord record;
        record.descriptor = descriptor.value();
        record.base_all_free = free_ranges_cover_chunk(record.descriptor);
        result.push_back(std::move(record));
    }
    return base::Result<std::vector<ChainRecord>>::success(std::move(result));
}

void append_overlay(std::vector<ChainRecord>& records, const std::size_t layer_index,
                    const ports::ChunkDescriptor& overlay) {
    // logical_offset is per source volume (restarts at 0); match source_index first.
    const auto overlay_end = overlay.logical_offset + overlay.logical_size;
    for (auto& record : records) {
        if (record.descriptor.source_index != overlay.source_index) {
            continue;
        }
        const auto base_start = record.descriptor.logical_offset;
        const auto base_end = base_start + record.descriptor.logical_size;
        if (base_end <= overlay.logical_offset) {
            continue;
        }
        if (base_start >= overlay_end) {
            break;
        }
        const auto start = (std::max)(base_start, overlay.logical_offset);
        const auto end = (std::min)(base_end, overlay_end);
        OverlaySlice slice{layer_index, overlay.chunk_index, start - overlay.logical_offset,
                           start - base_start, end - start};
        record.overlays.push_back(slice);
        apply_free_overlay(record, overlay, slice);
    }
}

[[nodiscard]] base::Result<void> add_layer_overlays(std::vector<ChainRecord>& records,
                                                    const std::size_t layer_index,
                                                    const PersonalArchiveReader& layer) {
    for (std::uint64_t index = 0; index < layer.chunk_count(); ++index) {
        auto descriptor = layer.describe_chunk(index);
        if (!descriptor) {
            return base::Result<void>::failure(descriptor.error());
        }
        append_overlay(records, layer_index, descriptor.value());
    }
    return base::Result<void>::success();
}

/// Sub-slices of `slice` outside `covered` (sorted, disjoint target ranges of later layers).
[[nodiscard]] std::vector<OverlaySlice>
visible_parts(const OverlaySlice& slice, const std::vector<ports::ChunkFreeRange>& covered) {
    std::vector<OverlaySlice> parts;
    auto cursor = slice.target_offset;
    const auto slice_end = slice.target_offset + slice.size;
    const auto emit = [&](const std::uint64_t from, const std::uint64_t to) {
        if (from < to) {
            parts.push_back({slice.layer_index, slice.source_chunk_index,
                             slice.source_offset + (from - slice.target_offset), from, to - from});
        }
    };
    for (const auto& range : covered) {
        const auto range_end = range.offset + range.size;
        if (range_end <= cursor) {
            continue;
        }
        if (range.offset >= slice_end) {
            break;
        }
        emit(cursor, (std::min)(range.offset, slice_end));
        cursor = (std::max)(cursor, range_end);
    }
    emit(cursor, slice_end);
    return parts;
}

/// Adds a target range to a sorted, disjoint set, merging overlapping and adjacent ranges
/// (slices of different layers overlap, unlike FREE ranges).
void add_covered_range(std::vector<ports::ChunkFreeRange>& covered, ports::ChunkFreeRange added) {
    std::vector<ports::ChunkFreeRange> merged;
    merged.reserve(covered.size() + 1);
    auto added_end = added.offset + added.size;
    for (const auto& range : covered) {
        const auto range_end = range.offset + range.size;
        if (range_end < added.offset || range.offset > added_end) {
            merged.push_back(range);
            continue;
        }
        added.offset = (std::min)(added.offset, range.offset);
        added_end = (std::max)(added_end, range_end);
    }
    added.size = added_end - added.offset;
    merged.push_back(added);
    std::ranges::sort(merged, {}, &ports::ChunkFreeRange::offset);
    covered = std::move(merged);
}

/// Later layers win. Walk overlays newest-first and keep only the bytes no later layer
/// rewrites, so a read decodes an overlay chunk only when part of it stays visible. A
/// region rewritten by several incrementals then costs one decode instead of one per
/// layer. The FREE bookkeeping in append_overlay already ran on the untrimmed slices.
void prune_hidden_overlays(ChainRecord& record) {
    std::vector<ports::ChunkFreeRange> covered;
    std::vector<OverlaySlice> visible;
    for (auto it = record.overlays.rbegin(); it != record.overlays.rend(); ++it) {
        auto parts = visible_parts(*it, covered);
        visible.insert(visible.end(), parts.begin(), parts.end());
        add_covered_range(covered, {it->target_offset, it->size});
    }
    std::reverse(visible.begin(), visible.end());
    record.overlays = std::move(visible);
    record.overlays_cover_chunk = covered.size() == 1 && covered.front().offset == 0 &&
                                  covered.front().size == record.descriptor.logical_size;
    record.overlay_covered = std::move(covered);
}

[[nodiscard]] std::vector<VolumeRecords>
index_volume_records(const format::Manifest& manifest, const std::vector<ChainRecord>& records) {
    std::vector<VolumeRecords> volumes;
    volumes.reserve(manifest.volumes.size());
    for (const auto& volume : manifest.volumes) {
        volumes.push_back({volume.volume_index, volume.total_size, {}});
    }
    for (std::size_t index = 0; index < records.size(); ++index) {
        const auto& descriptor = records[index].descriptor;
        auto volume = std::find_if(volumes.begin(), volumes.end(), [&](const VolumeRecords& v) {
            return v.volume_index == descriptor.source_index;
        });
        if (volume == volumes.end()) {
            continue;
        }
        volume->spans.push_back({descriptor.logical_offset, descriptor.logical_size, index});
    }
    for (auto& volume : volumes) {
        std::ranges::sort(volume.spans, {}, &RecordSpan::logical_offset);
    }
    return volumes;
}

[[nodiscard]] ArchiveChainGeometry
summarize_geometry(const std::vector<ChainRecord>& records,
                   const std::vector<std::unique_ptr<PersonalArchiveReader>>& layers,
                   const std::uint64_t overlay_slices_raw) {
    ArchiveChainGeometry geometry;
    geometry.overlay_slices_raw = overlay_slices_raw;
    for (const auto& record : records) {
        geometry.overlay_slices_visible += record.overlays.size();
        ++geometry.base_chunks;
        geometry.base_logical_bytes += record.descriptor.logical_size;
        if (!record.base_needed()) {
            ++geometry.base_chunks_not_decoded;
        }
    }
    for (std::size_t index = 1; index < layers.size(); ++index) {
        const auto& layer = *layers[index];
        geometry.overlay_chunks += layer.chunk_count();
        for (std::uint64_t chunk = 0; chunk < layer.chunk_count(); ++chunk) {
            if (auto descriptor = layer.describe_chunk(chunk)) {
                geometry.overlay_logical_bytes += descriptor.value().logical_size;
            }
        }
    }
    return geometry;
}

// ---------------------------------------------------------------------------------------
// Range walking: which archive chunk serves which bytes of a request.

/// Calls `emit(PieceRange)` for every run of [local_begin, local_end) within `record`
/// that holds data: visible overlay slices first, then the base wherever no overlay
/// rewrites it. Bytes not emitted are zero (FREE or all-free base).
template <typename Emit>
base::Result<void> emit_record_pieces(const ChainRecord& record, const std::size_t record_index,
                                      const std::uint64_t local_begin,
                                      const std::uint64_t local_end,
                                      const std::uint64_t range_base, Emit&& emit) {
    for (const auto& slice : record.overlays) {
        const auto from = (std::max)(slice.target_offset, local_begin);
        const auto to = (std::min)(slice.target_offset + slice.size, local_end);
        if (from >= to) {
            continue;
        }
        auto emitted = emit(PieceRange{{slice.layer_index, slice.source_chunk_index},
                                       slice.source_offset + (from - slice.target_offset),
                                       range_base + (from - local_begin), to - from});
        if (!emitted) {
            return emitted;
        }
    }
    if (!record.base_needed()) {
        return base::Result<void>::success();
    }
    const detail::DecodedChunkKey base_key{0, record_index};
    auto cursor = local_begin;
    const auto emit_base = [&](const std::uint64_t from, const std::uint64_t to) {
        if (from >= to) {
            return base::Result<void>::success();
        }
        return emit(PieceRange{base_key, from, range_base + (from - local_begin), to - from});
    };
    for (const auto& covered : record.overlay_covered) {
        const auto covered_end = covered.offset + covered.size;
        if (covered_end <= cursor) {
            continue;
        }
        if (covered.offset >= local_end) {
            break;
        }
        if (auto emitted = emit_base(cursor, (std::min)(covered.offset, local_end)); !emitted) {
            return emitted;
        }
        cursor = (std::max)(cursor, covered_end);
    }
    return emit_base(cursor, local_end);
}

/// Walks [offset, offset + size) of `volume` (clamped to the volume size) and emits the
/// pieces holding data, in ascending volume order. Gaps between base chunks are zero.
template <typename Emit>
base::Result<void> walk_volume_range(const VolumeRecords& volume,
                                     const std::vector<ChainRecord>& records,
                                     const std::uint64_t offset, const std::uint64_t size,
                                     Emit&& emit) {
    const auto end = (std::min)(volume.volume_size, offset + size);
    auto cursor = offset;
    while (cursor < end) {
        const auto span =
            std::lower_bound(volume.spans.begin(), volume.spans.end(), cursor,
                             [](const RecordSpan& candidate, const std::uint64_t position) {
                                 return candidate.logical_offset + candidate.logical_size <=
                                        position;
                             });
        if (span == volume.spans.end()) {
            break;
        }
        if (cursor < span->logical_offset) {
            cursor = (std::min)(end, span->logical_offset);
            continue;
        }
        const auto span_end = span->logical_offset + span->logical_size;
        const auto local_begin = cursor - span->logical_offset;
        const auto local_end = (std::min)(end, span_end) - span->logical_offset;
        auto emitted = emit_record_pieces(records[span->record_index], span->record_index,
                                          local_begin, local_end, cursor - offset, emit);
        if (!emitted) {
            return emitted;
        }
        cursor = span->logical_offset + local_end;
    }
    return base::Result<void>::success();
}

/// Copies `piece.size` bytes of `payload` into `destination` after bounds validation.
[[nodiscard]] base::Result<void> copy_piece(const PieceRange& piece,
                                            const std::vector<std::byte>& payload,
                                            const std::span<std::byte> destination) {
    if (piece.payload_offset > payload.size() ||
        piece.size > payload.size() - piece.payload_offset ||
        piece.range_offset > destination.size() ||
        piece.size > destination.size() - piece.range_offset) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chain piece is out of range"));
    }
    std::memcpy(destination.data() + static_cast<std::size_t>(piece.range_offset),
                payload.data() + static_cast<std::size_t>(piece.payload_offset),
                static_cast<std::size_t>(piece.size));
    return base::Result<void>::success();
}

// ---------------------------------------------------------------------------------------
// Prefetch: decode the pieces a sequential reader is about to ask for.

/// Sequential-read detector per volume plus a bounded FIFO of piece keys drained by
/// worker threads. Heuristic state only; every field uses default atomic ordering.
///
/// The look-ahead adapts to how much of it pays off: the chain cannot see file boundaries,
/// so a consumer hopping between short files (random order) turns most of a 16 MiB
/// look-ahead into decodes nobody reads. When the recent waste ratio exceeds
/// kThrottleWasteRatio the look-ahead shrinks to kThrottledAheadBytes, just enough to have
/// the next piece decoding when a stream reaches its boundary; it widens again once the
/// ratio falls below kRestoreWasteRatio (a long sequential copy wastes almost nothing).
class RangePrefetcher final {
  public:
    /// A read whose start lies within this distance of the previous read's end continues a
    /// sequential stream (Dokan threads reorder neighbouring requests slightly).
    static constexpr std::uint64_t kSequentialTolerance = 4ULL * 1024ULL * 1024ULL;
    /// Replan once the stream advanced this far past the last plan.
    static constexpr std::uint64_t kPlanStep = 4ULL * 1024ULL * 1024ULL;
    /// Bytes ahead of the stream whose pieces are queued.
    static constexpr std::uint64_t kAheadBytes = 16ULL * 1024ULL * 1024ULL;
    static constexpr std::uint64_t kThrottledAheadBytes = 4ULL * 1024ULL * 1024ULL;
    static constexpr std::size_t kMaximumQueued = 16;
    /// Waste is judged over windows of this many resolved prefetches (consumed or evicted
    /// unused); percentages are integers out of 100.
    static constexpr std::uint64_t kWasteWindow = 8;
    static constexpr std::uint64_t kThrottleWasteRatio = 50;
    static constexpr std::uint64_t kRestoreWasteRatio = 25;

    using Load = std::function<void(const detail::DecodedChunkKey&)>;

    RangePrefetcher(const std::size_t volume_count, const std::size_t thread_count, Load load)
        : load_(std::move(load)), last_end_(volume_count), planned_to_(volume_count) {
        for (auto& end : last_end_) {
            end.store(kNoPosition);
        }
        for (std::size_t index = 0; index < thread_count; ++index) {
            threads_.emplace_back([this](const std::stop_token stop) { run(stop); });
        }
    }

    ~RangePrefetcher() {
        for (auto& thread : threads_) {
            thread.request_stop();
        }
        signal_.notify_all();
        threads_.clear();
    }

    RangePrefetcher(const RangePrefetcher&) = delete;
    RangePrefetcher& operator=(const RangePrefetcher&) = delete;

    /// Records a served read; returns the range to plan ahead when the stream is
    /// sequential and advanced past the last plan, or an empty range otherwise.
    [[nodiscard]] std::pair<std::uint64_t, std::uint64_t>
    note_read(const std::size_t volume_slot, const std::uint64_t offset,
              const std::uint64_t end) {
        const auto previous = last_end_[volume_slot].exchange(end);
        const bool sequential = previous != kNoPosition &&
                                offset <= previous + kSequentialTolerance &&
                                offset + kSequentialTolerance >= previous;
        if (!sequential) {
            planned_to_[volume_slot].store(0);
            return {0, 0};
        }
        if (end < planned_to_[volume_slot].load()) {
            return {0, 0};
        }
        planned_to_[volume_slot].store(end + kPlanStep);
        if (throttled_.load()) {
            ++throttled_plans_;
            return {end, kThrottledAheadBytes};
        }
        return {end, kAheadBytes};
    }

    /// Feeds the cumulative cache counters; decides the look-ahead once a window of
    /// resolved prefetches has accumulated.
    void observe(const std::uint64_t prefetch_consumed_total,
                 const std::uint64_t prefetch_unused_total) {
        const std::scoped_lock lock(policy_mutex_);
        window_consumed_ += prefetch_consumed_total - seen_consumed_;
        window_unused_ += prefetch_unused_total - seen_unused_;
        seen_consumed_ = prefetch_consumed_total;
        seen_unused_ = prefetch_unused_total;
        const auto resolved = window_consumed_ + window_unused_;
        if (resolved < kWasteWindow) {
            return;
        }
        const auto waste_percent = window_unused_ * 100U / resolved;
        if (waste_percent > kThrottleWasteRatio) {
            throttled_.store(true);
        } else if (waste_percent < kRestoreWasteRatio) {
            throttled_.store(false);
        }
        window_consumed_ = 0;
        window_unused_ = 0;
    }

    [[nodiscard]] std::uint64_t throttled_plans() const noexcept { return throttled_plans_.load(); }

    void enqueue(const detail::DecodedChunkKey& key) {
        {
            const std::scoped_lock lock(mutex_);
            const bool pending =
                std::find(queue_.begin(), queue_.end(), key) != queue_.end() ||
                std::find(active_.begin(), active_.end(), key) != active_.end();
            if (pending) {
                return;
            }
            if (queue_.size() >= kMaximumQueued) {
                queue_.pop_front();
            }
            queue_.push_back(key);
        }
        signal_.notify_one();
    }

  private:
    static constexpr std::uint64_t kNoPosition = (std::numeric_limits<std::uint64_t>::max)();

    void run(const std::stop_token stop) {
        for (;;) {
            detail::DecodedChunkKey key;
            {
                std::unique_lock lock(mutex_);
                const bool requested =
                    signal_.wait(lock, stop, [this] { return !queue_.empty(); });
                if (!requested) {
                    return;
                }
                key = queue_.front();
                queue_.pop_front();
                active_.push_back(key);
            }
            load_(key);
            const std::scoped_lock lock(mutex_);
            std::erase(active_, key);
        }
    }

    Load load_;
    std::vector<std::atomic<std::uint64_t>> last_end_;
    std::vector<std::atomic<std::uint64_t>> planned_to_;
    std::atomic<bool> throttled_{false};
    std::atomic<std::uint64_t> throttled_plans_{0};
    // policy_mutex_ guards the waste window and the last sampled totals.
    std::mutex policy_mutex_;
    std::uint64_t seen_consumed_{0};
    std::uint64_t seen_unused_{0};
    std::uint64_t window_consumed_{0};
    std::uint64_t window_unused_{0};
    std::mutex mutex_;
    std::condition_variable_any signal_;
    std::deque<detail::DecodedChunkKey> queue_;
    std::vector<detail::DecodedChunkKey> active_;
    // Declared last so the threads are joined before anything they use is destroyed.
    std::vector<std::jthread> threads_;
};

} // namespace

base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>
PersonalArchiveChainReader::open_layers(
    const ArchiveChainOpenRequest& request,
    const std::shared_ptr<detail::BlockWorkerPool>& block_workers) {
    if (request.layers.empty() || request.maximum_chain_depth == 0 ||
        request.layers.size() > request.maximum_chain_depth) {
        return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive chain request is invalid"));
    }
    std::vector<std::unique_ptr<PersonalArchiveReader>> result;
    result.reserve(request.layers.size());
    for (const auto& layer_request : request.layers) {
        auto layer = PersonalArchiveReader::open_with_workers(layer_request, block_workers);
        if (!layer) {
            return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::failure(
                layer.error());
        }
        result.push_back(std::move(layer).value());
    }
    return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::success(
        std::move(result));
}

struct PersonalArchiveChainReader::Impl final {
    explicit Impl(const std::uint64_t cache_budget_bytes) : cache(cache_budget_bytes) {}

    ~Impl() {
        // Stop prefetch threads before layers and cache go away.
        prefetcher.reset();
    }

    /// Decodes one archive chunk on the calling thread and accounts it by layer and origin.
    [[nodiscard]] base::Result<std::vector<std::byte>>
    decode_piece(const detail::DecodedChunkKey& key, const detail::DecodedChunkOrigin origin,
                 const base::CancellationToken& cancellation) {
        const auto started = std::chrono::steady_clock::now();
        auto chunk = layers[key.layer_index]->read_chunk(key.chunk_index, cancellation);
        const auto elapsed = microseconds_since(started);
        if (key.layer_index == 0) {
            ++counters.base_decodes;
            counters.base_decode_microseconds += elapsed;
        } else {
            ++counters.overlay_decodes;
            counters.overlay_decode_microseconds += elapsed;
        }
        if (origin == detail::DecodedChunkOrigin::kPrefetch) {
            ++counters.prefetch_decodes;
            counters.prefetch_decode_microseconds += elapsed;
        } else {
            ++counters.on_demand_decodes;
            counters.on_demand_decode_microseconds += elapsed;
        }
        if (!chunk) {
            return base::Result<std::vector<std::byte>>::failure(chunk.error());
        }
        return base::Result<std::vector<std::byte>>::success(std::move(chunk).value().payload);
    }

    /// Cached decode of one archive chunk; concurrent requests share a single decode.
    [[nodiscard]] base::Result<detail::DecodedChunkPayload>
    load_piece(const detail::DecodedChunkKey& key, const base::CancellationToken& cancellation) {
        auto lookup = cache.get_or_load(key, detail::DecodedChunkOrigin::kOnDemand, [&] {
            return decode_piece(key, detail::DecodedChunkOrigin::kOnDemand, cancellation);
        });
        if (!lookup) {
            return base::Result<detail::DecodedChunkPayload>::failure(lookup.error());
        }
        if (lookup.value().hit) {
            ++counters.piece_cache_hits;
        }
        if (lookup.value().waited) {
            ++counters.piece_load_waits;
        }
        return base::Result<detail::DecodedChunkPayload>::success(
            std::move(lookup.value().payload));
    }

    void prefetch_piece(const detail::DecodedChunkKey& key) {
        if (cache.contains(key)) {
            return;
        }
        auto loaded = cache.get_or_load(key, detail::DecodedChunkOrigin::kPrefetch, [&] {
            return decode_piece(key, detail::DecodedChunkOrigin::kPrefetch,
                                base::CancellationToken{});
        });
        if (!loaded) {
            ++counters.prefetch_failures;
        }
    }

    /// Queues the pieces of the stream ahead of a sequential reader.
    void plan_prefetch(const std::size_t volume_slot, const std::uint64_t offset,
                       const std::uint64_t end) {
        if (prefetcher == nullptr) {
            return;
        }
        const auto cache_stats = cache.statistics();
        prefetcher->observe(cache_stats.prefetch_consumed, cache_stats.unconsumed_evictions);
        const auto [ahead_offset, ahead_size] = prefetcher->note_read(volume_slot, offset, end);
        if (ahead_size == 0) {
            return;
        }
        (void)walk_volume_range(volumes[volume_slot], records, ahead_offset, ahead_size,
                                [this](const PieceRange& piece) {
                                    if (!cache.contains(piece.key)) {
                                        prefetcher->enqueue(piece.key);
                                    }
                                    return base::Result<void>::success();
                                });
    }

    format::Manifest manifest;
    std::vector<std::unique_ptr<PersonalArchiveReader>> layers;
    std::vector<ChainRecord> records;
    std::vector<VolumeRecords> volumes;
    ArchiveChainGeometry geometry;
    detail::DecodedChunkCache cache;
    ChainCounters counters;
    std::unique_ptr<RangePrefetcher> prefetcher;
};

PersonalArchiveChainReader::PersonalArchiveChainReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveChainReader::~PersonalArchiveChainReader() = default;

base::Result<std::unique_ptr<PersonalArchiveChainReader>>
PersonalArchiveChainReader::open(const ArchiveChainOpenRequest& request) {
    auto block_workers =
        std::make_shared<detail::BlockWorkerPool>(detail::default_block_worker_count());
    auto layers = open_layers(request, block_workers);
    if (!layers) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(layers.error());
    }
    auto valid = validate_chain(layers.value());
    if (!valid) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(valid.error());
    }
    auto records = make_base_records(*layers.value().front());
    if (!records) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(records.error());
    }
    for (std::size_t index = 1; index < layers.value().size(); ++index) {
        auto added = add_layer_overlays(records.value(), index, *layers.value()[index]);
        if (!added) {
            return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(
                added.error());
        }
    }
    std::uint64_t overlay_slices_raw = 0;
    for (auto& record : records.value()) {
        overlay_slices_raw += record.overlays.size();
        prune_hidden_overlays(record);
    }
    auto implementation = std::make_unique<Impl>(request.range_cache_budget_bytes);
    implementation->geometry =
        summarize_geometry(records.value(), layers.value(), overlay_slices_raw);
    implementation->manifest = layers.value().back()->manifest();
    implementation->volumes = index_volume_records(implementation->manifest, records.value());
    implementation->layers = std::move(layers).value();
    implementation->records = std::move(records).value();
    if (request.range_prefetch_threads > 0) {
        Impl* impl = implementation.get();
        implementation->prefetcher = std::make_unique<RangePrefetcher>(
            implementation->volumes.size(), request.range_prefetch_threads,
            [impl](const detail::DecodedChunkKey& key) { impl->prefetch_piece(key); });
    }
    return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::success(
        std::unique_ptr<PersonalArchiveChainReader>(
            new PersonalArchiveChainReader(std::move(implementation))));
}

const format::Manifest& PersonalArchiveChainReader::manifest() const noexcept {
    return implementation_->manifest;
}

std::uint64_t PersonalArchiveChainReader::logical_size_bytes() const noexcept {
    return implementation_->layers.front()->logical_size_bytes();
}

std::uint64_t PersonalArchiveChainReader::chunk_count() const noexcept {
    return implementation_->records.size();
}

base::Result<ports::ChunkDescriptor>
PersonalArchiveChainReader::describe_chunk(const std::uint64_t chunk_index) const {
    if (chunk_index >= implementation_->records.size()) {
        return base::Result<ports::ChunkDescriptor>::failure(
            error(base::ErrorCode::kNotFound, "archive chain chunk does not exist"));
    }
    return base::Result<ports::ChunkDescriptor>::success(
        implementation_->records[static_cast<std::size_t>(chunk_index)].descriptor);
}

base::Result<ports::ChunkData>
PersonalArchiveChainReader::read_chunk(const std::uint64_t chunk_index,
                                       const base::CancellationToken cancellation) {
    auto descriptor = describe_chunk(chunk_index);
    if (!descriptor) {
        return base::Result<ports::ChunkData>::failure(descriptor.error());
    }
    const auto record_index = static_cast<std::size_t>(chunk_index);
    const auto& record = implementation_->records[record_index];
    // The merged chunk is built in place on the base payload (moved, not copied, so the
    // sequential restore path pays no extra 64 MiB copy); zeros when the base is unneeded.
    std::vector<std::byte> payload;
    if (record.base_needed()) {
        auto base = implementation_->decode_piece({0, record_index},
                                                  detail::DecodedChunkOrigin::kOnDemand,
                                                  cancellation);
        if (!base) {
            return base::Result<ports::ChunkData>::failure(base.error());
        }
        payload = std::move(base).value();
    }
    payload.resize(static_cast<std::size_t>(record.descriptor.logical_size));
    for (const auto& slice : record.overlays) {
        auto source = implementation_->load_piece({slice.layer_index, slice.source_chunk_index},
                                                  cancellation);
        if (!source) {
            return base::Result<ports::ChunkData>::failure(source.error());
        }
        auto copied = copy_piece(PieceRange{{slice.layer_index, slice.source_chunk_index},
                                            slice.source_offset, slice.target_offset, slice.size},
                                 *source.value(), payload);
        if (!copied) {
            return base::Result<ports::ChunkData>::failure(copied.error());
        }
    }
    ++implementation_->counters.chunks_read;
    return base::Result<ports::ChunkData>::success({descriptor.value(), std::move(payload)});
}

base::Result<void> PersonalArchiveChainReader::read_range(
    const std::uint32_t volume_index, const std::uint64_t volume_offset,
    const std::span<std::byte> destination, const base::CancellationToken cancellation) {
    if (destination.empty()) {
        return base::Result<void>::success();
    }
    std::memset(destination.data(), 0, destination.size());
    const auto& volumes = implementation_->volumes;
    const auto volume = std::find_if(volumes.begin(), volumes.end(), [&](const VolumeRecords& v) {
        return v.volume_index == volume_index;
    });
    if (volume == volumes.end()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kNotFound, "volume index is missing from archive chain"));
    }
    const auto fill = [&](const PieceRange& piece) -> base::Result<void> {
        auto source = implementation_->load_piece(piece.key, cancellation);
        if (!source) {
            return base::Result<void>::failure(source.error());
        }
        return copy_piece(piece, *source.value(), destination);
    };
    auto filled = walk_volume_range(*volume, implementation_->records, volume_offset,
                                    destination.size(), fill);
    if (!filled) {
        return filled;
    }
    ++implementation_->counters.range_reads;
    implementation_->counters.range_bytes += destination.size();
    implementation_->plan_prefetch(static_cast<std::size_t>(volume - volumes.begin()),
                                   volume_offset, volume_offset + destination.size());
    return base::Result<void>::success();
}

const ArchiveChainGeometry& PersonalArchiveChainReader::geometry() const noexcept {
    return implementation_->geometry;
}

ArchiveChainReadStatistics PersonalArchiveChainReader::statistics() const noexcept {
    const auto& counters = implementation_->counters;
    ArchiveChainReadStatistics result;
    result.chunks_read = counters.chunks_read.load();
    result.range_reads = counters.range_reads.load();
    result.range_bytes = counters.range_bytes.load();
    result.piece_cache_hits = counters.piece_cache_hits.load();
    result.piece_load_waits = counters.piece_load_waits.load();
    result.base_decodes = counters.base_decodes.load();
    result.base_decode_microseconds = counters.base_decode_microseconds.load();
    result.overlay_decodes = counters.overlay_decodes.load();
    result.overlay_decode_microseconds = counters.overlay_decode_microseconds.load();
    result.on_demand_decodes = counters.on_demand_decodes.load();
    result.on_demand_decode_microseconds = counters.on_demand_decode_microseconds.load();
    result.prefetch_decodes = counters.prefetch_decodes.load();
    result.prefetch_decode_microseconds = counters.prefetch_decode_microseconds.load();
    result.prefetch_failures = counters.prefetch_failures.load();
    const auto cache = implementation_->cache.statistics();
    result.cache_evictions = cache.evictions;
    result.prefetch_unused = cache.unconsumed_evictions;
    result.prefetch_consumed = cache.prefetch_consumed;
    if (implementation_->prefetcher != nullptr) {
        result.prefetch_throttled_plans = implementation_->prefetcher->throttled_plans();
    }
    for (const auto& layer : implementation_->layers) {
        result.layers += layer->read_statistics();
    }
    return result;
}

} // namespace aegra::adapters::personal_archive
