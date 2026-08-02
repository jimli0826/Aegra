#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/progress.h"

#include <cstdint>
#include <string>

namespace aegra::pipeline {

struct VerifyPlan final {
    std::string job_id;
    std::string trace_id;
};

struct VerifySummary final {
    std::uint64_t logical_bytes{0};
    std::uint64_t verified_bytes{0};
    std::uint64_t chunk_count{0};
};

class VerifyPipeline final {
  public:
    VerifyPipeline(ports::IRecoveryPointReader& reader,
                   ports::IProgressSink* progress = nullptr) noexcept;

    [[nodiscard]] base::Result<VerifySummary>
    run(const VerifyPlan& plan, const base::CancellationToken& cancellation);

  private:
    ports::IRecoveryPointReader& reader_;
    ports::IProgressSink* progress_;
};

} // namespace aegra::pipeline
