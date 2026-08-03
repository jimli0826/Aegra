#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

enum class ServiceExitCode : int {
    kSucceeded = 0,
    kInvalidArguments = 20,
    kHostFailure = 21,
};

struct ServiceArguments final {
    bool once{false};
    std::string pipe_name{"control"};
};

[[nodiscard]] bool parse_arguments(const std::span<const char* const> arguments,
                                   ServiceArguments& result) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::string_view argument = arguments[index];
        if (argument == "--once") {
            result.once = true;
            continue;
        }
        if (argument == "--pipe" && index + 1 < arguments.size()) {
            result.pipe_name = arguments[++index];
            continue;
        }
        return false;
    }
    return true;
}

[[nodiscard]] aegra::apps::service::ServiceRuntimeInfo runtime_info() {
    return {
        .service_version = AEGRA_APPLICATION_VERSION,
        .capabilities = {"service.info"},
    };
}

[[nodiscard]] ServiceExitCode
run_once(aegra::adapters::windows_ipc::WindowsNamedPipeListener& listener,
         const aegra::apps::service::ServiceRuntimeInfo& runtime) {
    auto channel = listener.accept({});
    if (!channel) {
        return ServiceExitCode::kHostFailure;
    }
    auto result = aegra::apps::service::run_service_session(*channel.value(), runtime, {}, 1);
    return result ? ServiceExitCode::kSucceeded : ServiceExitCode::kHostFailure;
}

[[nodiscard]] ServiceExitCode
run_forever(aegra::adapters::windows_ipc::WindowsNamedPipeListener& listener,
            const aegra::apps::service::ServiceRuntimeInfo& runtime) {
    for (;;) {
        auto channel = listener.accept({});
        if (!channel) {
            return ServiceExitCode::kHostFailure;
        }
        (void)aegra::apps::service::run_service_session(*channel.value(), runtime, {});
    }
}

[[nodiscard]] ServiceExitCode run_service(const std::span<const char* const> arguments) {
    ServiceArguments parsed;
    if (!parse_arguments(arguments, parsed)) {
        return ServiceExitCode::kInvalidArguments;
    }
    auto listener = aegra::adapters::windows_ipc::WindowsNamedPipeListener::create(
        {parsed.pipe_name,
         static_cast<std::uint32_t>(aegra::apps::service::kMaximumServiceFrameBytes)});
    if (!listener) {
        return ServiceExitCode::kHostFailure;
    }
    const auto runtime = runtime_info();
    return parsed.once ? run_once(*listener.value(), runtime)
                       : run_forever(*listener.value(), runtime);
}

} // namespace

int main(const int argument_count, const char* const* arguments) noexcept {
    try {
        return static_cast<int>(run_service({arguments, static_cast<std::size_t>(argument_count)}));
    } catch (...) {
        return static_cast<int>(ServiceExitCode::kHostFailure);
    }
}
