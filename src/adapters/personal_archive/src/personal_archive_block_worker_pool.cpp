#include "personal_archive_block_worker_pool.h"

#include <algorithm>
#include <utility>

namespace aegra::adapters::personal_archive::detail {
namespace {

[[nodiscard]] base::Error internal_error(std::string message) {
    return {base::ErrorCode::kInternal, std::move(message)};
}

void record_first_failure(std::atomic<bool>& failed, std::mutex& error_mutex,
                          base::Error& first_error, base::Error error) {
    const std::scoped_lock error_lock(error_mutex);
    if (!failed.exchange(true, std::memory_order_relaxed)) {
        first_error = std::move(error);
    }
}

} // namespace

std::uint32_t default_block_worker_count() noexcept {
    const auto hardware = std::thread::hardware_concurrency();
    return hardware == 0 ? 4U : hardware;
}

BlockWorkerPool::BlockWorkerPool(const std::uint32_t worker_count)
    : worker_count_((std::max)(1U, worker_count)) {
    workers_.resize(worker_count_);
    try {
        for (auto& slot : workers_) {
            slot.thread = std::thread([this, &slot] { worker_main(slot.local); });
        }
    } catch (...) {
        stop_workers();
        throw;
    }
}

BlockWorkerPool::~BlockWorkerPool() { stop_workers(); }

void BlockWorkerPool::stop_workers() noexcept {
    {
        const std::scoped_lock lock(mutex_);
        stop_ = true;
        job_active_ = false;
        work_ = {};
        // Wake workers blocked on a new epoch or job completion.
        ++job_epoch_;
    }
    work_cv_.notify_all();
    done_cv_.notify_all();
    for (auto& slot : workers_) {
        if (slot.thread.joinable()) {
            slot.thread.join();
        }
    }
    workers_.clear();
}

void BlockWorkerPool::worker_main(BlockWorkerLocal& local) {
    std::uint64_t seen_epoch = 0;
    for (;;) {
        WorkFn work;
        std::size_t job_work_count = 0;
        {
            std::unique_lock lock(mutex_);
            // Wait for a *new* job epoch (or stop). job_active_ alone is insufficient: a worker
            // that finishes early must not re-enter the same epoch while others still run.
            work_cv_.wait(lock, [this, seen_epoch] {
                return stop_ || job_epoch_ != seen_epoch;
            });
            if (stop_ && (!job_active_ || job_epoch_ == seen_epoch)) {
                return;
            }
            seen_epoch = job_epoch_;
            if (!job_active_) {
                // Stop or cancelled publish without work.
                continue;
            }
            job_work_count = work_count_;
            try {
                work = work_;
            } catch (...) {
                record_first_failure(failed_, error_mutex_, first_error_,
                                     internal_error("archive block worker failed to acquire work"));
            }
        }

        try {
            for (;;) {
                if (failed_.load(std::memory_order_relaxed)) {
                    break;
                }
                const auto index = next_index_.fetch_add(1, std::memory_order_relaxed);
                if (index >= job_work_count) {
                    break;
                }
                if (!work) {
                    record_first_failure(failed_, error_mutex_, first_error_,
                                         internal_error("archive block worker has no work function"));
                    break;
                }
                auto step = work(index, local);
                if (!step) {
                    record_first_failure(failed_, error_mutex_, first_error_, step.error());
                    break;
                }
            }
        } catch (...) {
            record_first_failure(failed_, error_mutex_, first_error_,
                                 internal_error("archive block worker failed unexpectedly"));
        }

        {
            std::unique_lock lock(mutex_);
            ++workers_finished_;
            if (workers_finished_ == worker_count_) {
                job_active_ = false;
                work_ = {};
                work_count_ = 0;
                done_cv_.notify_all();
            } else {
                // Stay out of the next epoch wait until every peer finished this epoch.
                done_cv_.wait(lock, [this, seen_epoch] {
                    return stop_ || !job_active_ || job_epoch_ != seen_epoch;
                });
            }
            if (stop_ && !job_active_) {
                return;
            }
        }
    }
}

base::Result<void> BlockWorkerPool::parallel_for(const std::size_t work_count, WorkFn work) {
    if (work_count == 0) {
        return base::Result<void>::success();
    }
    if (!work) {
        return base::Result<void>::failure(
            internal_error("archive block worker pool requires a work function"));
    }
    if (workers_.empty()) {
        return base::Result<void>::failure(
            internal_error("archive block worker pool is not running"));
    }

    {
        std::unique_lock lock(mutex_);
        done_cv_.wait(lock, [this] { return !job_active_; });
        if (stop_) {
            return base::Result<void>::failure(
                internal_error("archive block worker pool is stopping"));
        }
        failed_.store(false, std::memory_order_relaxed);
        first_error_ = {};
        next_index_.store(0, std::memory_order_relaxed);
        workers_finished_ = 0;
        work_count_ = work_count;
        work_ = std::move(work);
        job_active_ = true;
        ++job_epoch_;
    }
    work_cv_.notify_all();

    {
        std::unique_lock lock(mutex_);
        done_cv_.wait(lock, [this] { return !job_active_; });
        if (failed_.load(std::memory_order_relaxed)) {
            return base::Result<void>::failure(first_error_);
        }
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::personal_archive::detail
