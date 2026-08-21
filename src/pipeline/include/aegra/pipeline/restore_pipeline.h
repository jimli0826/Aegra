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

struct RestorePlan final {
    std::string job_id;
    std::string trace_id;
    std::size_t memory_budget_bytes{0};
    /// 0 = restore the full archive logical image (default).
    /// Non-zero = write only [0, logical_write_limit_bytes); full descriptor validation still runs.
    std::uint64_t logical_write_limit_bytes{0};
};

struct RestoreSummary final {
    std::uint64_t restored_bytes{0};
    std::uint64_t disk_written_bytes{0};
    std::uint64_t free_skipped_bytes{0};
    std::uint64_t free_range_count{0};
    std::uint64_t chunk_count{0};
    std::size_t peak_buffered_bytes{0};
};

class RestorePipeline final {
  public:
    // Dependencies are non-owning and must outlive run().
    RestorePipeline(ports::IRecoveryPointReader& reader, ports::IBlockSink& sink,
                    ports::IProgressSink* progress = nullptr) noexcept;

    [[nodiscard]] base::Result<RestoreSummary> run(const RestorePlan& plan,
                                                   const base::CancellationToken& cancellation);

  private:
    ports::IRecoveryPointReader& reader_;
    ports::IBlockSink& sink_;
    ports::IProgressSink* progress_;
};

} // namespace aegra::pipeline
