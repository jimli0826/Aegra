// Byte-bounded LRU of decoded archive chunks keyed by (layer, chunk), shared by every
// reader thread and the prefetcher of a chain reader.
//
// A slot is published before its decode starts, so concurrent requests for the same chunk
// join the in-flight load instead of decoding twice; a failed load is dropped so the next
// request retries and reports. Payloads are shared so a caller keeps its chunk alive after
// eviction. Eviction prefers slots a reader has already consumed over prefetched slots
// still waiting for their reader; a prefetched slot nobody touched within
// kUnconsumedGraceUses later uses is treated as stale (the reader went elsewhere) so a
// wasted prefetch cannot pin cache space. Budget 0 keeps nothing resident (dedupe only).
#pragma once

#include "aegra/base/error.h"
#include "aegra/base/result.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace aegra::adapters::personal_archive::detail {

struct DecodedChunkKey final {
    /// 0 is the base (full) layer; incremental layers follow in chain order.
    std::size_t layer_index{0};
    std::uint64_t chunk_index{0};

    [[nodiscard]] bool operator==(const DecodedChunkKey&) const noexcept = default;
};

enum class DecodedChunkOrigin : std::uint8_t { kOnDemand = 1, kPrefetch = 2 };

using DecodedChunkPayload = std::shared_ptr<const std::vector<std::byte>>;

class DecodedChunkCache final {
  public:
    using Loader = std::function<base::Result<std::vector<std::byte>>()>;

    struct Lookup final {
        DecodedChunkPayload payload;
        /// The slot already existed (decoded, or being decoded by another thread).
        bool hit{false};
        /// The caller blocked on a decode another thread had started.
        bool waited{false};
    };

    struct Statistics final {
        std::uint64_t evictions{0};
        /// Evicted slots no reader ever consumed: prefetches that went to waste.
        std::uint64_t unconsumed_evictions{0};
        /// Prefetched slots a reader did consume: prefetches that paid off.
        std::uint64_t prefetch_consumed{0};
        std::uint64_t resident_bytes{0};
    };

    /// Uses (hits and inserts on any slot) after which an unconsumed prefetch is stale.
    /// Reads are ~128 KiB, so this is roughly 128 MiB of traffic past the prefetch point.
    static constexpr std::uint64_t kUnconsumedGraceUses = 1024;

    explicit DecodedChunkCache(std::uint64_t budget_bytes);

    DecodedChunkCache(const DecodedChunkCache&) = delete;
    DecodedChunkCache& operator=(const DecodedChunkCache&) = delete;

    [[nodiscard]] bool contains(const DecodedChunkKey& key);

    /// Returns the decoded chunk, running `loader` on the calling thread when no other
    /// thread has claimed the key. The cache mutex is never held across `loader`.
    [[nodiscard]] base::Result<Lookup> get_or_load(const DecodedChunkKey& key,
                                                   DecodedChunkOrigin origin,
                                                   const Loader& loader);

    [[nodiscard]] Statistics statistics();

  private:
    struct Slot final {
        DecodedChunkKey key;
        bool ready{false};
        bool failed{false};
        /// A reader has taken this payload at least once.
        bool consumed{false};
        /// Inserted by a prefetch thread rather than a reader.
        bool prefetched{false};
        DecodedChunkPayload payload;
        base::Error error;
        std::uint64_t last_used{0};
    };

    [[nodiscard]] std::shared_ptr<Slot> find_locked(const DecodedChunkKey& key) const;
    [[nodiscard]] base::Result<Lookup> await_locked(std::unique_lock<std::mutex>& lock,
                                                    std::shared_ptr<Slot> slot);
    [[nodiscard]] std::shared_ptr<Slot> insert_loading_locked(const DecodedChunkKey& key,
                                                              DecodedChunkOrigin origin);
    void evict_locked(const Slot* protect);

    std::mutex mutex_;
    std::condition_variable ready_;
    std::uint64_t budget_bytes_{0};
    std::uint64_t resident_bytes_{0};
    std::uint64_t use_counter_{0};
    std::uint64_t evictions_{0};
    std::uint64_t unconsumed_evictions_{0};
    std::uint64_t prefetch_consumed_{0};
    std::vector<std::shared_ptr<Slot>> slots_;
};

} // namespace aegra::adapters::personal_archive::detail
