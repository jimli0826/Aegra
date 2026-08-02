#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <span>

namespace aegra::ports {

class IRandomSource {
  public:
    IRandomSource() = default;
    virtual ~IRandomSource() = default;
    IRandomSource(const IRandomSource&) = delete;
    IRandomSource& operator=(const IRandomSource&) = delete;
    IRandomSource(IRandomSource&&) = delete;
    IRandomSource& operator=(IRandomSource&&) = delete;

    [[nodiscard]] virtual base::Result<void> fill(std::span<std::byte> destination,
                                                  const base::CancellationToken& cancellation) = 0;
};

} // namespace aegra::ports
