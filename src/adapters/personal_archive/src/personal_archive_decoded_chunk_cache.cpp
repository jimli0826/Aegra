#include "personal_archive_decoded_chunk_cache.h"

#include <algorithm>
#include <utility>

namespace aegra::adapters::personal_archive::detail {

DecodedChunkCache::DecodedChunkCache(const std::uint64_t budget_bytes)
    : budget_bytes_(budget_bytes) {}

bool DecodedChunkCache::contains(const DecodedChunkKey& key) {
    const std::scoped_lock lock(mutex_);
    return find_locked(key) != nullptr;
}

base::Result<DecodedChunkCache::Lookup>
DecodedChunkCache::get_or_load(const DecodedChunkKey& key, const DecodedChunkOrigin origin,
                               const Loader& loader) {
    std::shared_ptr<Slot> slot;
    {
        std::unique_lock lock(mutex_);
        slot = find_locked(key);
        if (slot != nullptr) {
            return await_locked(lock, std::move(slot));
        }
        slot = insert_loading_locked(key, origin);
    }
    auto loaded = loader();
    const std::scoped_lock lock(mutex_);
    if (loaded) {
        slot->payload = std::make_shared<const std::vector<std::byte>>(std::move(loaded).value());
        resident_bytes_ += slot->payload->size();
    } else {
        slot->failed = true;
        slot->error = loaded.error();
        std::erase(slots_, slot);
    }
    slot->ready = true;
    ready_.notify_all();
    if (!loaded) {
        return base::Result<Lookup>::failure(loaded.error());
    }
    // A zero budget keeps nothing: the returned payload is the only reference left.
    evict_locked(budget_bytes_ == 0 ? nullptr : slot.get());
    return base::Result<Lookup>::success(Lookup{slot->payload, false, false});
}

std::shared_ptr<DecodedChunkCache::Slot>
DecodedChunkCache::find_locked(const DecodedChunkKey& key) const {
    for (const auto& slot : slots_) {
        if (slot->key == key) {
            return slot;
        }
    }
    return nullptr;
}

base::Result<DecodedChunkCache::Lookup>
DecodedChunkCache::await_locked(std::unique_lock<std::mutex>& lock, std::shared_ptr<Slot> slot) {
    slot->last_used = ++use_counter_;
    if (slot->prefetched && !slot->consumed) {
        ++prefetch_consumed_;
    }
    slot->consumed = true;
    const bool waited = !slot->ready;
    ready_.wait(lock, [&slot] { return slot->ready; });
    if (slot->failed) {
        return base::Result<Lookup>::failure(slot->error);
    }
    return base::Result<Lookup>::success(Lookup{slot->payload, true, waited});
}

std::shared_ptr<DecodedChunkCache::Slot>
DecodedChunkCache::insert_loading_locked(const DecodedChunkKey& key,
                                         const DecodedChunkOrigin origin) {
    auto slot = std::make_shared<Slot>();
    slot->key = key;
    slot->consumed = origin == DecodedChunkOrigin::kOnDemand;
    slot->prefetched = origin == DecodedChunkOrigin::kPrefetch;
    slot->last_used = ++use_counter_;
    slots_.push_back(slot);
    return slot;
}

DecodedChunkCache::Statistics DecodedChunkCache::statistics() {
    const std::scoped_lock lock(mutex_);
    return Statistics{evictions_, unconsumed_evictions_, prefetch_consumed_, resident_bytes_};
}

/// Drops least recently used decoded slots until the budget holds, never `protect` and
/// never a slot still loading (its size is unknown; it counts once ready). Consumed and
/// stale-unconsumed slots go first; a fresh prefetch is only evicted when nothing else can.
void DecodedChunkCache::evict_locked(const Slot* protect) {
    const auto pick = [this, protect](const bool spare_fresh_prefetch) {
        auto oldest = slots_.end();
        for (auto it = slots_.begin(); it != slots_.end(); ++it) {
            const Slot& slot = **it;
            const bool stale = use_counter_ - slot.last_used > kUnconsumedGraceUses;
            const bool eligible = slot.ready && &slot != protect &&
                                  (!spare_fresh_prefetch || slot.consumed || stale);
            const bool older = oldest == slots_.end() || slot.last_used < (*oldest)->last_used;
            if (eligible && older) {
                oldest = it;
            }
        }
        return oldest;
    };
    while (resident_bytes_ > budget_bytes_) {
        auto victim = pick(true);
        if (victim == slots_.end()) {
            victim = pick(false);
        }
        if (victim == slots_.end()) {
            return;
        }
        ++evictions_;
        if (!(*victim)->consumed) {
            ++unconsumed_evictions_;
        }
        resident_bytes_ -= (*victim)->payload->size();
        slots_.erase(victim);
    }
}

} // namespace aegra::adapters::personal_archive::detail
