#pragma once

#include "aegra/contracts/progress.h"

namespace aegra::ports {

class IProgressSink {
  public:
    IProgressSink() = default;
    virtual ~IProgressSink() = default;
    IProgressSink(const IProgressSink&) = delete;
    IProgressSink& operator=(const IProgressSink&) = delete;
    IProgressSink(IProgressSink&&) = delete;
    IProgressSink& operator=(IProgressSink&&) = delete;

    // Implementations must not throw. The pipeline may call publish from its consumer thread.
    virtual void publish(const contracts::TaskProgress& progress) noexcept = 0;
};

} // namespace aegra::ports
