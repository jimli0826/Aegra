#include "aegra/adapters/windows_ipc/windows_named_pipe_channel.h"
#include "aegra/apps/service/service_protocol.h"
#include "aegra/contracts/service.h"
#include "aegra/personal_repository/catalog.h"

#include <Windows.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {

namespace app = aegra::apps::service;
namespace contracts = aegra::contracts;
namespace repository = aegra::personal_repository;
namespace windows_ipc = aegra::adapters::windows_ipc;

constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFileUuid = "11111111-2222-4333-8444-555555555555";

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::wstring widen_ascii(const std::string& value) { return {value.begin(), value.end()}; }

std::string unique_pipe_name() {
    static std::atomic_uint32_t sequence{0};
    return "process-test-" + std::to_string(GetCurrentProcessId()) + "-" +
           std::to_string(sequence.fetch_add(1));
}

class ChildProcess final {
  public:
    ~ChildProcess() {
        if (process_ != nullptr) {
            if (!exited_) {
                TerminateProcess(process_, 99);
                (void)WaitForSingleObject(process_, 5'000);
            }
            CloseHandle(process_);
        }
        std::error_code ignored;
        std::filesystem::remove_all(data_dir_, ignored);
    }

    ChildProcess(const ChildProcess&) = delete;
    ChildProcess& operator=(const ChildProcess&) = delete;
    ChildProcess(ChildProcess&& other) noexcept
        : process_(std::exchange(other.process_, nullptr)),
          exited_(std::exchange(other.exited_, true)),
          data_dir_(std::exchange(other.data_dir_, {})) {}
    ChildProcess& operator=(ChildProcess&&) = delete;

    [[nodiscard]] static ChildProcess start(const std::wstring& executable,
                                            const std::string& pipe_name,
                                            const std::filesystem::path& repository_root = {}) {
        ChildProcess child;
        child.data_dir_ = std::filesystem::temp_directory_path() / "AegraServiceProcessTests" /
                          widen_ascii(unique_pipe_name());
        std::error_code directory_error;
        std::filesystem::create_directories(child.data_dir_, directory_error);
        if (directory_error) {
            return child;
        }
        std::wstring command = L"\"" + executable + L"\" --once --pipe " + widen_ascii(pipe_name);
        command += L" --data-dir \"" + child.data_dir_.wstring() + L"\"";
        if (!repository_root.empty()) {
            command += L" --repository-root \"" + repository_root.wstring() + L"\"";
        }
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
            CloseHandle(process.hThread);
            child.process_ = process.hProcess;
        }
        return child;
    }

    [[nodiscard]] bool valid() const noexcept { return process_ != nullptr; }

    [[nodiscard]] bool wait_for_success() {
        if (WaitForSingleObject(process_, 5'000) != WAIT_OBJECT_0) {
            return false;
        }
        exited_ = true;
        DWORD exit_code = 0;
        return GetExitCodeProcess(process_, &exit_code) && exit_code == 0;
    }

  private:
    ChildProcess() = default;

    HANDLE process_{nullptr};
    bool exited_{false};
    std::filesystem::path data_dir_;
};

class TemporaryRepository final {
  public:
    ~TemporaryRepository() {
        std::error_code ignored;
        std::filesystem::remove_all(root_, ignored);
    }

    TemporaryRepository(const TemporaryRepository&) = delete;
    TemporaryRepository& operator=(const TemporaryRepository&) = delete;
    TemporaryRepository(TemporaryRepository&& other) noexcept
        : root_(std::exchange(other.root_, {})) {}
    TemporaryRepository& operator=(TemporaryRepository&&) = delete;

    [[nodiscard]] static std::optional<TemporaryRepository> create() {
        auto root = std::filesystem::temp_directory_path() / "AegraServiceProcessTests" /
                    widen_ascii(unique_pipe_name());
        std::error_code error;
        std::filesystem::create_directories(root / "catalog" / "recovery-points", error);
        if (error) {
            return std::nullopt;
        }
        TemporaryRepository fixture(std::move(root));
        return fixture.seed() ? std::optional<TemporaryRepository>(std::move(fixture))
                              : std::nullopt;
    }

    [[nodiscard]] const std::filesystem::path& root() const noexcept { return root_; }

  private:
    explicit TemporaryRepository(std::filesystem::path root) : root_(std::move(root)) {}

    [[nodiscard]] static bool write(const std::filesystem::path& path,
                                    const std::string_view contents) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        return stream.good();
    }

    [[nodiscard]] bool seed() const {
        repository::RepositoryDescriptor descriptor;
        descriptor.repository_uuid = kRepositoryUuid;
        repository::CatalogEntry entry;
        entry.repository_uuid = kRepositoryUuid;
        entry.file_uuid = kFileUuid;
        entry.backup_set_uuid = kSetUuid;
        entry.backup_type = aegra::format::BackupType::kFull;
        entry.archive_main_key = std::string("archives/2026/08/") + kFileUuid + ".bkf";
        entry.logical_size_bytes = 8'589'934'592ULL;
        entry.stored_size_bytes = 4'294'967'296ULL;
        entry.source_count = 1;
        auto encoded_descriptor = repository::encode_repository_descriptor_json(descriptor);
        auto encoded_entry = repository::encode_catalog_entry_json(entry);
        return encoded_descriptor && encoded_entry &&
               write(root_ / "aegra.repository", encoded_descriptor.value()) &&
               write(root_ / "catalog" / "recovery-points" / (std::string(kFileUuid) + ".entry"),
                     encoded_entry.value());
    }

    std::filesystem::path root_;
};

[[nodiscard]] std::optional<contracts::ServiceResponse>
send_request(const std::string& pipe_name, const contracts::ServiceRequest& request) {
    auto channel = windows_ipc::WindowsNamedPipeChannel::connect(
        {pipe_name, 5'000, static_cast<std::uint32_t>(app::kMaximumServiceFrameBytes),
         windows_ipc::WindowsNamedPipeNamespace::kService},
        {});
    if (!channel) {
        return std::nullopt;
    }
    auto encoded = app::encode_service_request(request);
    if (!encoded || !channel.value()->send(encoded.value(), {})) {
        return std::nullopt;
    }
    auto received = channel.value()->receive({});
    if (!received) {
        return std::nullopt;
    }
    auto response = app::decode_service_response(received.value());
    return response ? std::optional<contracts::ServiceResponse>(std::move(response).value())
                    : std::nullopt;
}

bool test_once_process(const std::wstring& service_executable) {
    const auto pipe_name = unique_pipe_name();
    auto process = ChildProcess::start(service_executable, pipe_name);
    if (!expect(process.valid(), "service child process starts")) {
        return false;
    }
    contracts::ServiceRequest request;
    request.request_id = "process-request-1";
    auto response = send_request(pipe_name, request);
    const auto* service =
        response ? std::get_if<contracts::ServiceInfo>(&response->payload) : nullptr;
    bool passed = expect(response && response->request_id == "process-request-1" &&
                             service != nullptr && service->service_version == "0.1.0",
                         "service process returns correlated Ready info");
    passed &= expect(process.wait_for_success(), "--once service exits successfully");
    return passed;
}

bool test_repository_process(const std::wstring& service_executable) {
    auto fixture = TemporaryRepository::create();
    if (!expect(fixture.has_value(), "temporary repository fixture is created")) {
        return false;
    }
    const auto pipe_name = unique_pipe_name();
    auto process = ChildProcess::start(service_executable, pipe_name, fixture->root());
    if (!expect(process.valid(), "repository service child process starts")) {
        return false;
    }
    contracts::ServiceRequest request;
    request.request_id = "repository-process-request-1";
    request.kind = contracts::ServiceRequestKind::kListRecoveryPoints;
    request.payload = contracts::ServiceRecoveryPointListRequest{std::nullopt, {100, std::nullopt}};
    auto response = send_request(pipe_name, request);
    const auto* service_page =
        response ? std::get_if<contracts::ServiceRecoveryPointPage>(&response->payload) : nullptr;
    const auto* page = service_page != nullptr ? &service_page->catalog : nullptr;
    const auto valid_page = page != nullptr &&
                            page->state == contracts::RepositoryCatalogState::kCatalogReady &&
                            page->repository_uuid == kRepositoryUuid && page->items.size() == 1 &&
                            page->items.front().file_uuid == kFileUuid;
    bool passed = expect(valid_page, "service reads the local repository catalog through IPC");
    passed &= expect(process.wait_for_success(), "repository --once service exits successfully");
    return passed;
}

} // namespace

int wmain(const int argument_count, wchar_t* arguments[]) noexcept {
    try {
        if (argument_count != 2) {
            std::fputs("[FAIL] expected aegra_service executable path\n", stderr);
            return EXIT_FAILURE;
        }
        return test_once_process(arguments[1]) && test_repository_process(arguments[1])
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
