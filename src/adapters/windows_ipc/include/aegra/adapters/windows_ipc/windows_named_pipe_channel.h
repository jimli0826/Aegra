#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/message_channel.h"

#include <cstdint>
#include <memory>
#include <string>

namespace aegra::adapters::windows_ipc {

enum class WindowsNamedPipeNamespace : std::uint8_t {
    kWorker = 1,
    kService = 2,
};

struct WindowsNamedPipeConnectRequest final {
    std::string pipe_name;
    std::uint32_t connect_timeout_ms{10'000};
    std::uint32_t maximum_frame_bytes{1024U * 1024U};
    WindowsNamedPipeNamespace pipe_namespace{WindowsNamedPipeNamespace::kWorker};
};

struct WindowsNamedPipeListenRequest final {
    std::string pipe_name;
    std::uint32_t maximum_frame_bytes{64U * 1024U};
};

class WindowsNamedPipeListener;

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

    friend class WindowsNamedPipeListener;

    explicit WindowsNamedPipeChannel(std::unique_ptr<Impl> impl);
    [[nodiscard]] static std::unique_ptr<WindowsNamedPipeChannel>
    adopt_connected(std::uintptr_t native_handle, std::uint32_t maximum_frame_bytes);

    std::unique_ptr<Impl> impl_;
};

class WindowsNamedPipeListener final {
  public:
    [[nodiscard]] static base::Result<std::unique_ptr<WindowsNamedPipeListener>>
    create(const WindowsNamedPipeListenRequest& request);

    ~WindowsNamedPipeListener();
    WindowsNamedPipeListener(const WindowsNamedPipeListener&) = delete;
    WindowsNamedPipeListener& operator=(const WindowsNamedPipeListener&) = delete;
    WindowsNamedPipeListener(WindowsNamedPipeListener&&) = delete;
    WindowsNamedPipeListener& operator=(WindowsNamedPipeListener&&) = delete;

    [[nodiscard]] base::Result<std::unique_ptr<WindowsNamedPipeChannel>>
    accept(const base::CancellationToken& cancellation);

  private:
    explicit WindowsNamedPipeListener(WindowsNamedPipeListenRequest request);

    WindowsNamedPipeListenRequest request_;
};

} // namespace aegra::adapters::windows_ipc
