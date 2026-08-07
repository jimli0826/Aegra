#include "mount_host_session.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/base/cancellation.h"

#include <Windows.h>

#include <iostream>
#include <string>
#include <string_view>

namespace {

[[nodiscard]] int usage() {
    std::cerr << "Usage: aegra_mount_host --pipe <logical-pipe-name>\n";
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    std::string pipe_name;
    for (int index = 1; index < argc; ++index) {
        const std::wstring arg = argv[index] != nullptr ? argv[index] : L"";
        if (arg == L"--pipe" && index + 1 < argc) {
            const auto* value = argv[++index];
            if (value == nullptr) {
                return usage();
            }
            const auto required =
                WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, nullptr, 0, nullptr,
                                    nullptr);
            if (required <= 1) {
                return usage();
            }
            pipe_name.assign(static_cast<std::size_t>(required - 1), '\0');
            if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value, -1, pipe_name.data(),
                                    required, nullptr, nullptr) == 0) {
                return usage();
            }
            continue;
        }
        return usage();
    }
    if (pipe_name.empty()) {
        return usage();
    }

    aegra::adapters::windows_ipc::WindowsNamedPipeConnectRequest request;
    request.pipe_name = pipe_name;
    request.maximum_frame_bytes = 1024U * 1024U;
    request.pipe_namespace = aegra::adapters::windows_ipc::WindowsNamedPipeNamespace::kWorker;
    request.connect_timeout_ms = 30'000;

    aegra::base::CancellationSource cancel;
    auto channel =
        aegra::adapters::windows_ipc::WindowsNamedPipeChannel::connect(request, cancel.get_token());
    if (!channel) {
        std::cerr << "mount host failed to connect to service pipe\n";
        return 1;
    }

    auto result =
        aegra::apps::mount_host::run_mount_host_session(*channel.value(), cancel.get_token());
    return result ? 0 : 1;
}
