#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/message_channel.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aegra::adapters::windows_ipc {

struct WindowsNamedPipeConnectRequest final {
    std::string pipe_name;
    std::uint32_t connect_timeout_ms{10'000};
    std::uint32_t maximum_frame_bytes{1024U * 1024U};
};

class WindowsNamedPipeChannel final : public ports::IMessageChannel {
  public:
    ~WindowsNamedPipeChannel() override;
    WindowsNamedPipeChannel(const WindowsNamedPipeChannel&) = delete;
    WindowsNamedPipeChannel& operator=(const WindowsNamedPipeChannel&) = delete;
    WindowsNamedPipeChannel(WindowsNamedPipeChannel&&) = delete;
    WindowsNamedPipeChannel& operator=(WindowsNamedPipeChannel&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WindowsNamedPipeChannel>>
    connect(const WindowsNamedPipeConnectRequest& request,
            const base::CancellationToken& cancellation);

    [[nodiscard]] base::Result<std::string>
    receive(const base::CancellationToken& cancellation) override;
    [[nodiscard]] base::Result<void> send(std::string_view message,
                                          const base::CancellationToken& cancellation) override;

  private:
    struct Impl;

    explicit WindowsNamedPipeChannel(std::unique_ptr<Impl> impl);

    std::unique_ptr<Impl> impl_;
};

} // namespace aegra::adapters::windows_ipc
