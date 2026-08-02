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

struct BackupPlan final {
    std::string job_id;
    std::string trace_id;
    std::uint64_t chunk_size_bytes{0};
    std::size_t memory_budget_bytes{0};
};

struct BackupSummary final {
    std::uint64_t logical_bytes{0};
    std::uint64_t stored_bytes{0};
    std::uint64_t chunk_count{0};
    std::size_t peak_buffered_bytes{0};
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
