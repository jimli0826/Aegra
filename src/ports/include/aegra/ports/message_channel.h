#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <string>
#include <string_view>

namespace aegra::ports {

class IMessageChannel {
  public:
    IMessageChannel() = default;
    virtual ~IMessageChannel() = default;
    IMessageChannel(const IMessageChannel&) = delete;
    IMessageChannel& operator=(const IMessageChannel&) = delete;
    IMessageChannel(IMessageChannel&&) = delete;
    IMessageChannel& operator=(IMessageChannel&&) = delete;

    // One reader and one writer may operate concurrently. Messages are owned UTF-8 frames.
    // Implementations must cap frame size and make pending I/O respond to cancellation.
    [[nodiscard]] virtual base::Result<std::string>
    receive(const base::CancellationToken& cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> send(std::string_view message,
                                                  const base::CancellationToken& cancellation) = 0;
};

} // namespace aegra::ports
