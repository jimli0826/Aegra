#pragma once

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "windows_cng_sha256.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace aegra::adapters::personal_archive::detail {

/// Per-worker resources reused across every chunk of a PersonalArchiveSession.
struct BlockWorkerLocal final {
    WindowsCngSha256 hasher;
    compression_zstd::ZstdCompressor compressor;
    compression_zstd::ZstdDecompressor decompressor;
    /// Compression scratch sized to the block compress bound; grown once, reused per block.
    std::vector<std::byte> scratch;
};

/// Session-scoped worker pool: threads live for the session, not per physical chunk.
/// At most one `parallel_for` runs at a time (PersonalArchiveSession is single-writer).
/// Each worker participates in a given job epoch exactly once (no re-entry while active).
class BlockWorkerPool final {
  public:
    using WorkFn =
        std::function<base::Result<void>(std::size_t index, BlockWorkerLocal& local)>;

    explicit BlockWorkerPool(std::uint32_t worker_count);
    ~BlockWorkerPool();
    BlockWorkerPool(const BlockWorkerPool&) = delete;
    BlockWorkerPool& operator=(const BlockWorkerPool&) = delete;
    BlockWorkerPool(BlockWorkerPool&&) = delete;
    BlockWorkerPool& operator=(BlockWorkerPool&&) = delete;

    [[nodiscard]] std::uint32_t worker_count() const noexcept { return worker_count_; }

    /// Fan-out `work_count` items over the pool. Empty work succeeds immediately.
    [[nodiscard]] base::Result<void> parallel_for(std::size_t work_count, WorkFn work);

  private:
    struct WorkerSlot final {
        std::thread thread;
        BlockWorkerLocal local;
    };

    void worker_main(BlockWorkerLocal& local);
    void stop_workers() noexcept;

    const std::uint32_t worker_count_;
    std::vector<WorkerSlot> workers_;

    std::mutex mutex_;
    std::condition_variable work_cv_;
    std::condition_variable done_cv_;
    bool stop_{false};
    bool job_active_{false};
    /// Bumped for every published job so workers cannot re-enter the same job.
    std::uint64_t job_epoch_{0};
    std::size_t work_count_{0};
    std::atomic<std::size_t> next_index_{0};
    std::uint32_t workers_finished_{0};
    WorkFn work_;
    std::atomic<bool> failed_{false};
    base::Error first_error_{};
    std::mutex error_mutex_;
};

[[nodiscard]] std::uint32_t default_block_worker_count() noexcept;

} // namespace aegra::adapters::personal_archive::detail
