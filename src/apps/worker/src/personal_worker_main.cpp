#include "aegra/apps/worker/worker_protocol.h"
#include "aegra/apps/worker/worker_session.h"

#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/base/cancellation.h"

#include <Windows.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace {

constexpr std::size_t kMaximumRequestBytes = std::size_t{1024} * 1024U;
constexpr std::string_view kStaticHostFailure =
    R"({"schema_version":1,"job_id":"","trace_id":"","kind":3,"boundary_error_code":9,"message_code":"worker.host_failed","task_result":null})";

std::string to_utf8(const std::wstring& value) {
    const auto maximum_input = static_cast<std::size_t>((std::numeric_limits<int>::max)());
    if (value.empty() || value.size() > maximum_input) {
        return {};
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
                                              input_size, nullptr, 0, nullptr, nullptr);
    if (required == 0) {
        return {};
    }
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), input_size, output.data(),
                            required, nullptr, nullptr) == 0) {
        return {};
    }
    return output;
}

std::string hostname() {
    std::array<wchar_t, 256> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    if (!GetComputerNameExW(ComputerNameDnsHostname, buffer.data(), &size)) {
        return {};
    }
    return to_utf8(std::wstring(buffer.data(), size));
}

aegra::apps::worker::WindowsPersonalBackupTaskOptions trusted_options() {
    aegra::apps::worker::WindowsPersonalBackupTaskOptions options;
    // Match old AegraImage engine constants (backup_format.h / constants.h):
    // BLOCK_SIZE=64KiB, READ_CHUNK_SIZE=64MiB. Hash/compress still fan out across
    // hardware_concurrency workers inside PersonalArchive chunk preparation.
    options.block_size_bytes = 64U * 1024U;
    options.chunk_size_bytes = 64U * 1024U * 1024U;
    // Keep several 64MiB chunks in flight without approaching old 512MiB DEFAULT_CHUNK.
    options.memory_budget_bytes = std::size_t{256} * 1024U * 1024U;
    options.application_version = AEGRA_APPLICATION_VERSION;
    options.hostname = hostname();
    return options;
}

std::string read_request() {
    std::string request;
    request.reserve(4096);
    std::array<char, 4096> buffer{};
    while (std::cin.good() && request.size() <= kMaximumRequestBytes) {
        std::cin.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        request.append(buffer.data(), static_cast<std::size_t>(std::cin.gcount()));
    }
    return request;
}

struct Runtime final {
    aegra::adapters::windows_system::WindowsCredentialResolver credentials;
    aegra::adapters::windows_system::WindowsCryptographicRandom random;
    aegra::adapters::windows_system::WindowsSystemClock clock;
    aegra::apps::worker::WindowsPersonalBackupTaskOptions options{trusted_options()};

    [[nodiscard]] aegra::apps::worker::WindowsPersonalBackupTaskContext context() {
        return {credentials, random, clock, nullptr};
    }
};

int run_stdin_worker(Runtime& runtime) {
    auto context = runtime.context();
    auto result = aegra::apps::worker::run_windows_personal_backup_worker_request(
        read_request(), runtime.options, context, {});
    if (!result) {
        return static_cast<int>(aegra::apps::worker::WorkerExitCode::kHostFailure);
    }
    std::cout << result.value().response_json << '\n';
    return static_cast<int>(result.value().exit_code);
}

int run_pipe_worker(Runtime& runtime, const std::string_view pipe_name) {
    auto channel = aegra::adapters::windows_ipc::WindowsNamedPipeChannel::connect(
        {std::string(pipe_name)}, {});
    if (!channel) {
        return static_cast<int>(aegra::apps::worker::WorkerExitCode::kHostFailure);
    }
    auto context = runtime.context();
    const auto exit = aegra::apps::worker::run_windows_personal_backup_worker_session(
        *channel.value(), runtime.options, context, {});
    return static_cast<int>(exit);
}

int run_worker(const std::span<const char* const> arguments) {
    Runtime runtime;
    if (arguments.size() == 3 && std::string_view(arguments[1]) == "--pipe") {
        return run_pipe_worker(runtime, arguments[2]);
    }
    if (arguments.size() == 1) {
        return run_stdin_worker(runtime);
    }
    return static_cast<int>(aegra::apps::worker::WorkerExitCode::kRequestRejected);
}

} // namespace

int main(const int argument_count, const char* const* arguments) noexcept {
    try {
        return run_worker({arguments, static_cast<std::size_t>(argument_count)});
    } catch (...) {
        const auto output = GetStdHandle(STD_OUTPUT_HANDLE);
        DWORD written = 0;
        WriteFile(output, kStaticHostFailure.data(), static_cast<DWORD>(kStaticHostFailure.size()),
                  &written, nullptr);
        return static_cast<int>(aegra::apps::worker::WorkerExitCode::kHostFailure);
    }
}
