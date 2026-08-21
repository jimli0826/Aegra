#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/block_io.h"

#include <cstdint>
#include <span>
#include <vector>

namespace aegra::pipeline {

/// Half-open byte range [begin, end).
struct ProtectedByteRange final {
    std::uint64_t begin{0};
    std::uint64_t end{0};
};

/// IBlockSink view that silently skips writes overlapping protected ranges.
/// Used so prefix restore cannot overwrite Primary/Backup Boot or plan-declared escrow sectors.
class ProtectedRangeBlockSink final : public ports::IBlockSink {
  public:
    ProtectedRangeBlockSink(ports::IBlockSink& inner,
                            std::vector<ProtectedByteRange> protected_ranges) noexcept;

    [[nodiscard]] std::uint64_t capacity_bytes() const noexcept override;
    [[nodiscard]] base::Result<void> write(std::uint64_t offset, std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override;

  private:
    ports::IBlockSink* inner_;
    std::vector<ProtectedByteRange> protected_ranges_;
};

} // namespace aegra::pipeline
