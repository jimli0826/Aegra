#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/block_io.h"
#include "aegra/ports/progress.h"

#include <cstddef>
#include <cstdint>
#include <string>

namespace aegra::pipeline {

enum class BackupCommitMode : std::uint8_t {
    kDefer = 1,
    kCommit = 2,
};

struct BackupPlan final {
    std::string job_id;
    std::string trace_id;
    std::uint64_t chunk_size_bytes{0};
    std::size_t memory_budget_bytes{0};
    std::uint32_t source_index{0};
    BackupCommitMode commit_mode{BackupCommitMode::kCommit};
    /// Multi-volume job totals for progress (0 = use this source size only).
    std::uint64_t progress_total_logical_bytes{0};
    /// Bytes already finished on prior volumes (processed / stored).
    std::uint64_t progress_base_processed_bytes{0};
    std::uint64_t progress_base_stored_bytes{0};
};

struct BackupSummary final {
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t chunk_count{0};
    std::size_t peak_buffered_bytes{0};
    std::uint64_t producer_read_microseconds{0};
    std::uint64_t producer_payload_allocate_microseconds{0};
    std::uint64_t producer_buffer_wait_microseconds{0};
    std::uint64_t producer_extent_describe_microseconds{0};
    std::uint64_t producer_source_read_microseconds{0};
    std::uint64_t producer_source_read_bytes{0};
    std::uint64_t producer_free_bytes{0};
    std::uint64_t producer_extent_describe_calls{0};
    std::uint64_t producer_source_read_calls{0};
    std::uint64_t producer_queue_wait_microseconds{0};
    std::uint64_t consumer_queue_wait_microseconds{0};
    std::uint64_t consumer_write_microseconds{0};
    std::uint64_t consumer_progress_microseconds{0};
};

class BackupPipeline final {
  public:
    // Dependencies are non-owning and must outlive run().
    BackupPipeline(ports::IBlockSource& source, ports::IBackupSession& session,
                   ports::IProgressSink* progress = nullptr) noexcept;

    [[nodiscard]] base::Result<BackupSummary> run(const BackupPlan& plan,
                                                  const base::CancellationToken& cancellation);

  private:
    ports::IBlockSource& source_;
    ports::IBackupSession& session_;
    ports::IProgressSink* progress_;
};

} // namespace aegra::pipeline
