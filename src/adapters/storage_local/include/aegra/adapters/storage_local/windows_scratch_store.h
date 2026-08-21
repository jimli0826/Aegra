#pragma once

#include "aegra/ports/scratch_store.h"

#include <memory>

namespace aegra::adapters::storage_local {

/// Windows sparse-file scratch store factory (ADR-0025 / SR2).
/// Public header intentionally omits Windows.h.
class WindowsScratchStoreFactory final : public ports::IScratchStoreFactory {
  public:
    WindowsScratchStoreFactory() = default;
    ~WindowsScratchStoreFactory() override = default;
    WindowsScratchStoreFactory(const WindowsScratchStoreFactory&) = delete;
    WindowsScratchStoreFactory& operator=(const WindowsScratchStoreFactory&) = delete;
    WindowsScratchStoreFactory(WindowsScratchStoreFactory&&) = delete;
    WindowsScratchStoreFactory& operator=(WindowsScratchStoreFactory&&) = delete;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IScratchStore>>
    open(const ports::ScratchStoreOpenRequest& request,
         base::CancellationToken cancellation) override;
};

} // namespace aegra::adapters::storage_local
