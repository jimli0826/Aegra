#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <list>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

[[nodiscard]] base::Error make_error(const base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

struct VolumeChunkSpan final {
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
    std::uint64_t inner_chunk_index{0};
};

struct CachedChunk final {
    std::uint64_t inner_chunk_index{(std::numeric_limits<std::uint64_t>::max)()};
    std::vector<std::byte> payload;
};

// Fixed-capacity LRU for decompressed chunk payloads (product-bounded).
class ChunkPayloadCache final {
  public:
    explicit ChunkPayloadCache(const std::size_t capacity)
        : capacity_(capacity == 0 ? 1 : capacity) {}

    [[nodiscard]] std::optional<std::span<const std::byte>> try_get(const std::uint64_t index) {
        auto it = index_.find(index);
        if (it == index_.end()) {
            return std::nullopt;
        }
        order_.splice(order_.begin(), order_, it->second);
        return std::span<const std::byte>(it->second->second);
    }

    void put(const std::uint64_t index, std::vector<std::byte> payload) {
        auto it = index_.find(index);
        if (it != index_.end()) {
            it->second->second = std::move(payload);
            order_.splice(order_.begin(), order_, it->second);
            return;
        }
        order_.emplace_front(index, std::move(payload));
        index_[index] = order_.begin();
        while (index_.size() > capacity_) {
            auto last = std::prev(order_.end());
            index_.erase(last->first);
            order_.pop_back();
        }
    }

  private:
    using List = std::list<std::pair<std::uint64_t, std::vector<std::byte>>>;
    std::size_t capacity_;
    List order_;
    std::unordered_map<std::uint64_t, typename List::iterator> index_;
};

[[nodiscard]] const format::Volume* find_volume(const format::Manifest& manifest,
                                                const std::uint32_t volume_index) noexcept {
    for (const auto& volume : manifest.volumes) {
        if (volume.volume_index == volume_index) {
            return &volume;
        }
    }
    return nullptr;
}

[[nodiscard]] base::Result<std::vector<VolumeChunkSpan>>
build_volume_chunks(ports::IRecoveryPointReader& inner, const std::uint32_t volume_index,
                    const std::uint64_t volume_size) {
    std::vector<VolumeChunkSpan> chunks;
    for (std::uint64_t chunk_index = 0; chunk_index < inner.chunk_count(); ++chunk_index) {
        auto descriptor = inner.describe_chunk(chunk_index);
        if (!descriptor) {
            return base::Result<std::vector<VolumeChunkSpan>>::failure(descriptor.error());
        }
        if (descriptor.value().source_index != volume_index) {
            continue;
        }
        if (descriptor.value().logical_size == 0) {
            return base::Result<std::vector<VolumeChunkSpan>>::failure(
                make_error(base::ErrorCode::kCorruptData, "chunk logical size is zero"));
        }
        std::uint64_t end = 0;
        if (descriptor.value().logical_offset >
                (std::numeric_limits<std::uint64_t>::max)() - descriptor.value().logical_size ||
            descriptor.value().logical_offset + descriptor.value().logical_size > volume_size) {
            return base::Result<std::vector<VolumeChunkSpan>>::failure(
                make_error(base::ErrorCode::kCorruptData, "volume chunk exceeds volume size"));
        }
        end = descriptor.value().logical_offset + descriptor.value().logical_size;
        (void)end;
        chunks.push_back(VolumeChunkSpan{
            .logical_offset = descriptor.value().logical_offset,
            .logical_size = descriptor.value().logical_size,
            .inner_chunk_index = chunk_index,
        });
    }
    std::sort(chunks.begin(), chunks.end(),
              [](const VolumeChunkSpan& left, const VolumeChunkSpan& right) {
                  return left.logical_offset < right.logical_offset;
              });
    // Reject overlaps; gaps are allowed and zero-filled (FREE / unmapped).
    for (std::size_t i = 1; i < chunks.size(); ++i) {
        const auto& prev = chunks[i - 1];
        const auto& cur = chunks[i];
        if (cur.logical_offset < prev.logical_offset + prev.logical_size) {
            return base::Result<std::vector<VolumeChunkSpan>>::failure(
                make_error(base::ErrorCode::kCorruptData, "volume chunk descriptors overlap"));
        }
    }
    return base::Result<std::vector<VolumeChunkSpan>>::success(std::move(chunks));
}

} // namespace

struct PersonalArchiveVolumeRandomReader::Impl final {
    ports::IRecoveryPointReader* inner{nullptr};
    std::uint32_t volume_index{0};
    std::uint64_t volume_size{0};
    std::vector<VolumeChunkSpan> chunks;
    mutable std::mutex cache_mutex;
    mutable ChunkPayloadCache cache{8};
};

PersonalArchiveVolumeRandomReader::PersonalArchiveVolumeRandomReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveVolumeRandomReader::~PersonalArchiveVolumeRandomReader() = default;

base::Result<std::unique_ptr<PersonalArchiveVolumeRandomReader>>
PersonalArchiveVolumeRandomReader::open(ports::IRecoveryPointReader& inner,
                                        const format::Manifest& manifest,
                                        const std::uint32_t volume_index) {
    const auto* volume = find_volume(manifest, volume_index);
    if (volume == nullptr || volume->total_size == 0) {
        return base::Result<std::unique_ptr<PersonalArchiveVolumeRandomReader>>::failure(
            make_error(base::ErrorCode::kNotFound, "volume is missing from archive manifest"));
    }
    auto chunks = build_volume_chunks(inner, volume_index, volume->total_size);
    if (!chunks) {
        return base::Result<std::unique_ptr<PersonalArchiveVolumeRandomReader>>::failure(
            chunks.error());
    }
    auto implementation = std::make_unique<Impl>();
    implementation->inner = &inner;
    implementation->volume_index = volume_index;
    implementation->volume_size = volume->total_size;
    implementation->chunks = std::move(chunks).value();
    return base::Result<std::unique_ptr<PersonalArchiveVolumeRandomReader>>::success(
        std::unique_ptr<PersonalArchiveVolumeRandomReader>(
            new PersonalArchiveVolumeRandomReader(std::move(implementation))));
}

std::uint64_t PersonalArchiveVolumeRandomReader::size_bytes() const noexcept {
    return implementation_->volume_size;
}

std::uint32_t PersonalArchiveVolumeRandomReader::volume_index() const noexcept {
    return implementation_->volume_index;
}

base::Result<std::size_t>
PersonalArchiveVolumeRandomReader::read_at(const std::uint64_t offset,
                                           const std::span<std::byte> destination,
                                           const base::CancellationToken cancellation) {
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }
    if (offset >= implementation_->volume_size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto max_readable = implementation_->volume_size - offset;
    const auto to_read = max_readable < destination.size()
                             ? static_cast<std::size_t>(max_readable)
                             : destination.size();
    std::memset(destination.data(), 0, to_read);

    auto remaining = destination.subspan(0, to_read);
    auto cursor = offset;
    while (!remaining.empty() && cursor < implementation_->volume_size) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                make_error(base::ErrorCode::kCancelled, "volume random read cancelled"));
        }
        const auto chunk = std::lower_bound(
            implementation_->chunks.begin(), implementation_->chunks.end(), cursor,
            [](const VolumeChunkSpan& span, const std::uint64_t value) {
                return span.logical_offset + span.logical_size <= value;
            });
        if (chunk == implementation_->chunks.end() || cursor < chunk->logical_offset) {
            const auto gap_end = chunk == implementation_->chunks.end()
                                     ? implementation_->volume_size
                                     : chunk->logical_offset;
            const auto skip =
                (std::min)(static_cast<std::uint64_t>(remaining.size()), gap_end - cursor);
            remaining = remaining.subspan(static_cast<std::size_t>(skip));
            cursor += skip;
            continue;
        }

        std::span<const std::byte> payload_view;
        {
            std::lock_guard lock(implementation_->cache_mutex);
            if (auto hit = implementation_->cache.try_get(chunk->inner_chunk_index); hit) {
                payload_view = *hit;
            }
        }
        if (payload_view.empty()) {
            auto loaded =
                implementation_->inner->read_chunk(chunk->inner_chunk_index, cancellation);
            if (!loaded) {
                return base::Result<std::size_t>::failure(loaded.error());
            }
            if (loaded.value().payload.size() < chunk->logical_size) {
                return base::Result<std::size_t>::failure(make_error(
                    base::ErrorCode::kCorruptData, "chunk payload is shorter than logical size"));
            }
            std::lock_guard lock(implementation_->cache_mutex);
            implementation_->cache.put(chunk->inner_chunk_index,
                                       std::move(loaded).value().payload);
            auto hit = implementation_->cache.try_get(chunk->inner_chunk_index);
            if (!hit) {
                return base::Result<std::size_t>::failure(
                    make_error(base::ErrorCode::kInternal, "chunk cache insert failed"));
            }
            payload_view = *hit;
        }

        const auto into_chunk = cursor - chunk->logical_offset;
        const auto copy_size = (std::min)(static_cast<std::uint64_t>(remaining.size()),
                                          chunk->logical_size - into_chunk);
        std::memcpy(remaining.data(), payload_view.data() + into_chunk,
                    static_cast<std::size_t>(copy_size));
        remaining = remaining.subspan(static_cast<std::size_t>(copy_size));
        cursor += copy_size;
    }
    return base::Result<std::size_t>::success(to_read);
}

} // namespace aegra::adapters::personal_archive
