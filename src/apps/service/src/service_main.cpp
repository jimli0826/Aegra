#include "aegra/adapters/storage_local/local_object_storage.h"
#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/application/personal_repository_query.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/service_protocol.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace service = aegra::apps::service;
namespace storage_local = aegra::adapters::storage_local;
namespace windows_ipc = aegra::adapters::windows_ipc;

enum class ServiceExitCode : int {
    kSucceeded = 0,
    kInvalidArguments = 20,
    kHostFailure = 21,
};

struct ServiceArguments final {
    bool once{false};
    std::string pipe_name{"control"};
    std::optional<std::filesystem::path> repository_root;
};

struct RuntimeComponents final {
    std::unique_ptr<storage_local::LocalObjectStorage> storage;
    std::unique_ptr<aegra::application::PersonalRepositoryQuery> repository_query;
    service::ServiceRuntimeInfo runtime;
};

[[nodiscard]] std::optional<std::string> narrow_ascii(const std::wstring_view value) {
    std::string result;
    result.reserve(value.size());
    for (const auto character : value) {
        if (character > 0x7F) {
            return std::nullopt;
        }
        result.push_back(static_cast<char>(character));
    }
    return result;
}

[[nodiscard]] bool parse_arguments(const std::span<const wchar_t* const> arguments,
                                   ServiceArguments& result) {
    for (std::size_t index = 1; index < arguments.size(); ++index) {
        const std::wstring_view argument = arguments[index];
        if (argument == L"--once" && !result.once) {
            result.once = true;
            continue;
        }
        if (argument == L"--pipe" && index + 1 < arguments.size()) {
            auto pipe_name = narrow_ascii(arguments[++index]);
            if (!pipe_name) {
                return false;
            }
            result.pipe_name = std::move(*pipe_name);
            continue;
        }
        if (argument == L"--repository-root" && index + 1 < arguments.size() &&
            !result.repository_root) {
            result.repository_root = std::filesystem::path(arguments[++index]);
            continue;
        }
        return false;
    }
    return !result.repository_root || result.repository_root->is_absolute();
}

[[nodiscard]] aegra::base::Result<RuntimeComponents>
create_runtime(const ServiceArguments& arguments) {
    RuntimeComponents components;
    if (arguments.repository_root) {
        auto opened = storage_local::LocalObjectStorage::open(
            {*arguments.repository_root, storage_local::LocalRootMode::kOpenExisting});
        if (!opened) {
            return aegra::base::Result<RuntimeComponents>::failure(opened.error());
        }
        components.storage = std::move(opened).value();
        components.repository_query = std::make_unique<aegra::application::PersonalRepositoryQuery>(
            *components.storage, *components.storage);
    } else {
        components.repository_query =
            std::make_unique<aegra::application::PersonalRepositoryQuery>();
    }
    components.runtime = {
        .service_version = AEGRA_APPLICATION_VERSION,
        .capabilities = {"repository.list", "service.info"},
        .repository_query = components.repository_query.get(),
    };
    return aegra::base::Result<RuntimeComponents>::success(std::move(components));
}

[[nodiscard]] ServiceExitCode run_once(windows_ipc::WindowsNamedPipeListener& listener,
                                       const service::ServiceRuntimeInfo& runtime) {
    auto channel = listener.accept({});
    if (!channel) {
        return ServiceExitCode::kHostFailure;
    }
    auto result = service::run_service_session(*channel.value(), runtime, {}, 1);
    return result ? ServiceExitCode::kSucceeded : ServiceExitCode::kHostFailure;
}

[[nodiscard]] ServiceExitCode run_forever(windows_ipc::WindowsNamedPipeListener& listener,
                                          const service::ServiceRuntimeInfo& runtime) {
    for (;;) {
        auto channel = listener.accept({});
        if (!channel) {
            return ServiceExitCode::kHostFailure;
        }
        (void)service::run_service_session(*channel.value(), runtime, {});
    }
}

[[nodiscard]] ServiceExitCode run_service(const std::span<const wchar_t* const> arguments) {
    ServiceArguments parsed;
    if (!parse_arguments(arguments, parsed)) {
        return ServiceExitCode::kInvalidArguments;
    }
    auto runtime = create_runtime(parsed);
    if (!runtime) {
        return ServiceExitCode::kHostFailure;
    }
    auto listener = windows_ipc::WindowsNamedPipeListener::create(
        {parsed.pipe_name, static_cast<std::uint32_t>(service::kMaximumServiceFrameBytes)});
    if (!listener) {
        return ServiceExitCode::kHostFailure;
    }
    return parsed.once ? run_once(*listener.value(), runtime.value().runtime)
                       : run_forever(*listener.value(), runtime.value().runtime);
}

} // namespace

int wmain(const int argument_count, const wchar_t* const* arguments) noexcept {
    try {
        return static_cast<int>(run_service({arguments, static_cast<std::size_t>(argument_count)}));
    } catch (...) {
        return static_cast<int>(ServiceExitCode::kHostFailure);
    }
}
