#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace aegra::ports {

/// Bounded scratch store for ShrinkPlan spool and sparse overlay pages.
/// Implementations must enforce hard allocation quotas and detect corrupt pages.
class IScratchStore {
  public:
    IScratchStore() = default;
    virtual ~IScratchStore() = default;
    IScratchStore(const IScratchStore&) = delete;
    IScratchStore& operator=(const IScratchStore&) = delete;
    IScratchStore(IScratchStore&&) = delete;
    IScratchStore& operator=(IScratchStore&&) = delete;

    /// Logical address space size in bytes (may exceed allocated physical bytes).
    [[nodiscard]] virtual std::uint64_t logical_size_bytes() const noexcept = 0;

    /// Physical bytes currently allocated to back the logical space.
    [[nodiscard]] virtual std::uint64_t allocated_bytes() const noexcept = 0;

    /// Hard upper bound on allocated_bytes(); exceeding it fails closed.
    [[nodiscard]] virtual std::uint64_t maximum_allocation_bytes() const noexcept = 0;

    [[nodiscard]] virtual base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void>
    write_at(std::uint64_t offset, std::span<const std::byte> source,
             base::CancellationToken cancellation) = 0;

    [[nodiscard]] virtual base::Result<void> flush(base::CancellationToken cancellation) = 0;

    /// Verifies page checksums / integrity for the half-open range [offset, offset+length).
    [[nodiscard]] virtual base::Result<void>
    verify_range(std::uint64_t offset, std::uint64_t length,
                 base::CancellationToken cancellation) = 0;

    /// Releases resources; subsequent I/O fails. Discard may delete backing storage.
    [[nodiscard]] virtual base::Result<void> close_and_discard() = 0;
};

struct ScratchStoreOpenRequest final {
    /// Absolute UTF-8 path for the scratch backing file. Must not reside on the restore target.
    std::string path_utf8;
    std::uint64_t logical_size_bytes{0};
    std::uint64_t maximum_allocation_bytes{0};
    /// Soft cap on in-memory overlay/index structures; overflow uses on-disk index segments.
    std::uint64_t memory_budget_bytes{0};
    /// Canonical Volume GUID path (with trailing slash) that scratch must NOT reside on.
    /// Empty = skip this check (caller responsible). UTF-8.
    std::string forbidden_volume_guid_utf8;
};

class IScratchStoreFactory {
  public:
    IScratchStoreFactory() = default;
    virtual ~IScratchStoreFactory() = default;
    IScratchStoreFactory(const IScratchStoreFactory&) = delete;
    IScratchStoreFactory& operator=(const IScratchStoreFactory&) = delete;
    IScratchStoreFactory(IScratchStoreFactory&&) = delete;
    IScratchStoreFactory& operator=(IScratchStoreFactory&&) = delete;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IScratchStore>>
    open(const ScratchStoreOpenRequest& request, base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
