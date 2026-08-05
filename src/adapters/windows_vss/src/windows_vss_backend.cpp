#include "vss_snapshot_core.h"

#include "com_ptr.h"

// VSS SDK headers are order-dependent: vsbackup consumes declarations from vss and vswriter.
// clang-format off
#include <Windows.h>
#include <combaseapi.h>
#include <oleauto.h>
#include <vss.h>
#include <vswriter.h>
#include <vsbackup.h>
#include <vsserror.h>
// clang-format on

#include <condition_variable>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_vss::detail {
namespace {

base::Error hresult_error(const HRESULT result, std::string operation) {
    auto code = base::ErrorCode::kIoFailure;
    if (result == E_ACCESSDENIED || result == HRESULT_FROM_WIN32(ERROR_PRIVILEGE_NOT_HELD)) {
        code = base::ErrorCode::kUnauthorized;
    } else if (result == VSS_E_OBJECT_NOT_FOUND) {
        code = base::ErrorCode::kNotFound;
    } else if (result == VSS_E_INSUFFICIENT_STORAGE) {
        code = base::ErrorCode::kInsufficientSpace;
    } else if (result == VSS_E_VOLUME_NOT_SUPPORTED ||
               result == VSS_E_VOLUME_NOT_SUPPORTED_BY_PROVIDER) {
        code = base::ErrorCode::kInvalidArgument;
    } else if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
        code = base::ErrorCode::kCancelled;
    }
    operation += " failed with HRESULT ";
    operation += std::to_string(static_cast<std::uint32_t>(result));
    return base::Error{code, std::move(operation)};
}

class ComApartment final {
  public:
    ComApartment(const ComApartment&) = delete;
    ComApartment& operator=(const ComApartment&) = delete;
    ComApartment(ComApartment&&) = delete;
    ComApartment& operator=(ComApartment&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<ComApartment>> enter() {
        const auto result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result)) {
            return base::Result<std::unique_ptr<ComApartment>>::failure(
                hresult_error(result, "CoInitializeEx"));
        }
        return base::Result<std::unique_ptr<ComApartment>>::success(
            std::unique_ptr<ComApartment>(new ComApartment()));
    }

    ~ComApartment() { CoUninitialize(); }

  private:
    ComApartment() = default;
};

base::Result<void> initialize_com_security() {
    const auto result =
        CoInitializeSecurity(nullptr, -1, nullptr, nullptr, RPC_C_AUTHN_LEVEL_PKT_PRIVACY,
                             RPC_C_IMP_LEVEL_IDENTIFY, nullptr, EOAC_NONE, nullptr);
    if (SUCCEEDED(result) || result == RPC_E_TOO_LATE) {
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(hresult_error(result, "CoInitializeSecurity"));
}

base::Result<void> wait_for_async(IVssAsync& operation, const base::CancellationToken& cancellation,
                                  const std::string& operation_name) {
    constexpr DWORD kPollIntervalMilliseconds = 25;
    for (;;) {
        HRESULT status = VSS_S_ASYNC_PENDING;
        const auto query_result = operation.QueryStatus(&status, nullptr);
        if (FAILED(query_result)) {
            return base::Result<void>::failure(hresult_error(query_result, operation_name));
        }
        if (status != VSS_S_ASYNC_PENDING) {
            if (FAILED(status)) {
                return base::Result<void>::failure(hresult_error(status, operation_name));
            }
            return base::Result<void>::success();
        }
        if (cancellation.stop_requested()) {
            operation.Cancel();
            operation.Wait();
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kCancelled, "VSS operation cancelled"});
        }
        Sleep(kPollIntervalMilliseconds);
    }
}

base::Result<void> wait_for_async(IVssAsync& operation, const std::string& operation_name) {
    return wait_for_async(operation, {}, operation_name);
}

class VssRequester final {
  public:
    VssRequester() = default;
    ~VssRequester() { abandon(); }

    VssRequester(const VssRequester&) = delete;
    VssRequester& operator=(const VssRequester&) = delete;
    VssRequester(VssRequester&&) = delete;
    VssRequester& operator=(VssRequester&&) = delete;

    [[nodiscard]] base::Result<std::vector<WindowsVssSnapshot>>
    create(const std::span<const WindowsVssSnapshotRequest> requests,
           const base::CancellationToken& cancellation) {
        auto initialized = initialize();
        if (!initialized) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(initialized.error());
        }
        auto gathered = gather_writer_metadata(cancellation);
        if (!gathered) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(gathered.error());
        }
        auto added = add_snapshots(requests);
        if (!added) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(added.error());
        }
        auto prepared = prepare_for_backup(cancellation);
        if (!prepared) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(prepared.error());
        }
        auto prepared_status = check_writer_status(cancellation);
        if (!prepared_status) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(prepared_status.error());
        }
        auto created = create_snapshot_set(cancellation);
        if (!created) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(created.error());
        }
        auto snapshot_status = check_writer_status(cancellation);
        if (!snapshot_status) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(snapshot_status.error());
        }
        return collect_snapshots(requests);
    }

    [[nodiscard]] base::Result<void> close() {
        auto completed = complete_backup();
        auto deleted = delete_snapshot_set();
        if (deleted) {
            release_components();
        }
        if (!completed) {
            return completed;
        }
        return deleted;
    }

    void abandon() noexcept {
        if (components_) {
            components_->AbortBackup();
            if (!IsEqualGUID(snapshot_set_id_, GUID_NULL)) {
                LONG deleted = 0;
                VSS_ID nondeleted = GUID_NULL;
                components_->DeleteSnapshots(snapshot_set_id_, VSS_OBJECT_SNAPSHOT_SET, true,
                                             &deleted, &nondeleted);
                snapshot_set_id_ = GUID_NULL;
                snapshot_ids_.clear();
            }
            release_components();
        }
    }

  private:
    [[nodiscard]] base::Result<void> initialize();
    [[nodiscard]] base::Result<void>
    gather_writer_metadata(const base::CancellationToken& cancellation);
    [[nodiscard]] base::Result<void>
    add_snapshots(std::span<const WindowsVssSnapshotRequest> requests);
    [[nodiscard]] base::Result<void>
    prepare_for_backup(const base::CancellationToken& cancellation);
    [[nodiscard]] base::Result<void>
    create_snapshot_set(const base::CancellationToken& cancellation);
    [[nodiscard]] base::Result<void>
    check_writer_status(const base::CancellationToken& cancellation);
    [[nodiscard]] base::Result<std::vector<WindowsVssSnapshot>>
    collect_snapshots(std::span<const WindowsVssSnapshotRequest> requests);
    [[nodiscard]] base::Result<void> complete_backup();
    [[nodiscard]] base::Result<void> delete_snapshot_set();
    void release_components() noexcept;

    ComPtr<IVssBackupComponents> components_;
    VSS_ID snapshot_set_id_{GUID_NULL};
    std::vector<VSS_ID> snapshot_ids_;
    bool writer_metadata_gathered_{false};
    bool backup_completed_{false};
};

class SnapshotProperties final {
  public:
    SnapshotProperties() = default;
    ~SnapshotProperties() { VssFreeSnapshotProperties(&value); }

    SnapshotProperties(const SnapshotProperties&) = delete;
    SnapshotProperties& operator=(const SnapshotProperties&) = delete;
    SnapshotProperties(SnapshotProperties&&) = delete;
    SnapshotProperties& operator=(SnapshotProperties&&) = delete;

    VSS_SNAPSHOT_PROP value{};
};

class WriterStatusScope final {
  public:
    explicit WriterStatusScope(IVssBackupComponents& components) noexcept
        : components_(components) {}
    ~WriterStatusScope() { components_.FreeWriterStatus(); }

    WriterStatusScope(const WriterStatusScope&) = delete;
    WriterStatusScope& operator=(const WriterStatusScope&) = delete;
    WriterStatusScope(WriterStatusScope&&) = delete;
    WriterStatusScope& operator=(WriterStatusScope&&) = delete;

  private:
    IVssBackupComponents& components_;
};

class UniqueBstr final {
  public:
    UniqueBstr() = default;
    ~UniqueBstr() { SysFreeString(value_); }

    UniqueBstr(const UniqueBstr&) = delete;
    UniqueBstr& operator=(const UniqueBstr&) = delete;
    UniqueBstr(UniqueBstr&&) = delete;
    UniqueBstr& operator=(UniqueBstr&&) = delete;

    [[nodiscard]] BSTR* put() noexcept { return &value_; }

  private:
    BSTR value_{nullptr};
};

base::Result<void> VssRequester::initialize() {
    auto security = initialize_com_security();
    if (!security) {
        return security;
    }
    auto result = CreateVssBackupComponents(components_.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "CreateVssBackupComponents"));
    }
    result = components_->InitializeForBackup();
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "InitializeForBackup"));
    }
    result = components_->SetContext(VSS_CTX_BACKUP);
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "SetContext"));
    }
    result = components_->SetBackupState(false, false, VSS_BT_FULL, false);
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "SetBackupState"));
    }
    return base::Result<void>::success();
}

base::Result<void>
VssRequester::gather_writer_metadata(const base::CancellationToken& cancellation) {
    ComPtr<IVssAsync> operation;
    const auto result = components_->GatherWriterMetadata(operation.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "GatherWriterMetadata"));
    }
    writer_metadata_gathered_ = true;
    return wait_for_async(*operation.get(), cancellation, "GatherWriterMetadata");
}

base::Result<void>
VssRequester::add_snapshots(const std::span<const WindowsVssSnapshotRequest> requests) {
    auto result = components_->StartSnapshotSet(&snapshot_set_id_);
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "StartSnapshotSet"));
    }
    snapshot_ids_.reserve(requests.size());
    for (const auto& request : requests) {
        VSS_ID snapshot_id = GUID_NULL;
        auto volume = request.volume_guid_path.native();
        result = components_->AddToSnapshotSet(volume.data(), GUID_NULL, &snapshot_id);
        if (FAILED(result)) {
            return base::Result<void>::failure(hresult_error(result, "AddToSnapshotSet"));
        }
        snapshot_ids_.push_back(snapshot_id);
    }
    return base::Result<void>::success();
}

base::Result<void> VssRequester::prepare_for_backup(const base::CancellationToken& cancellation) {
    ComPtr<IVssAsync> operation;
    const auto result = components_->PrepareForBackup(operation.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "PrepareForBackup"));
    }
    return wait_for_async(*operation.get(), cancellation, "PrepareForBackup");
}

base::Result<void> VssRequester::create_snapshot_set(const base::CancellationToken& cancellation) {
    ComPtr<IVssAsync> operation;
    const auto result = components_->DoSnapshotSet(operation.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "DoSnapshotSet"));
    }
    return wait_for_async(*operation.get(), cancellation, "DoSnapshotSet");
}

base::Result<void> VssRequester::check_writer_status(const base::CancellationToken& cancellation) {
    ComPtr<IVssAsync> operation;
    auto result = components_->GatherWriterStatus(operation.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "GatherWriterStatus"));
    }
    const WriterStatusScope status_scope(*components_.get());
    auto waited = wait_for_async(*operation.get(), cancellation, "GatherWriterStatus");
    if (!waited) {
        return waited;
    }

    UINT count = 0;
    result = components_->GetWriterStatusCount(&count);
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "GetWriterStatusCount"));
    }
    for (UINT index = 0; index < count; ++index) {
        VSS_ID instance_id = GUID_NULL;
        VSS_ID writer_id = GUID_NULL;
        VSS_WRITER_STATE state = VSS_WS_UNKNOWN;
        HRESULT writer_failure = S_OK;
        UniqueBstr writer_name;
        result = components_->GetWriterStatus(index, &instance_id, &writer_id, writer_name.put(),
                                              &state, &writer_failure);
        if (FAILED(result)) {
            return base::Result<void>::failure(hresult_error(result, "GetWriterStatus"));
        }
        if (FAILED(writer_failure)) {
            return base::Result<void>::failure(hresult_error(writer_failure, "VSS writer status"));
        }
    }
    return base::Result<void>::success();
}

base::Result<std::vector<WindowsVssSnapshot>>
VssRequester::collect_snapshots(const std::span<const WindowsVssSnapshotRequest> requests) {
    std::vector<WindowsVssSnapshot> snapshots;
    snapshots.reserve(requests.size());
    for (std::size_t index = 0; index < requests.size(); ++index) {
        SnapshotProperties properties;
        const auto result =
            components_->GetSnapshotProperties(snapshot_ids_[index], &properties.value);
        if (FAILED(result)) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(
                hresult_error(result, "GetSnapshotProperties"));
        }
        if (properties.value.m_pwszSnapshotDeviceObject == nullptr) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(base::Error{
                base::ErrorCode::kInternal,
                "VSS snapshot is missing its device object",
            });
        }
        snapshots.push_back(WindowsVssSnapshot{
            requests[index].volume_guid_path,
            properties.value.m_pwszSnapshotDeviceObject,
            requests[index].logical_size_bytes,
        });
    }
    return base::Result<std::vector<WindowsVssSnapshot>>::success(std::move(snapshots));
}

base::Result<void> VssRequester::complete_backup() {
    if (!components_ || backup_completed_) {
        return base::Result<void>::success();
    }
    ComPtr<IVssAsync> operation;
    const auto result = components_->BackupComplete(operation.put());
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "BackupComplete"));
    }
    auto waited = wait_for_async(*operation.get(), "BackupComplete");
    if (waited) {
        backup_completed_ = true;
    }
    return waited;
}

base::Result<void> VssRequester::delete_snapshot_set() {
    if (!components_ || IsEqualGUID(snapshot_set_id_, GUID_NULL)) {
        return base::Result<void>::success();
    }
    LONG deleted = 0;
    VSS_ID nondeleted = GUID_NULL;
    const auto result = components_->DeleteSnapshots(snapshot_set_id_, VSS_OBJECT_SNAPSHOT_SET,
                                                     true, &deleted, &nondeleted);
    if (FAILED(result)) {
        return base::Result<void>::failure(hresult_error(result, "DeleteSnapshots"));
    }
    snapshot_set_id_ = GUID_NULL;
    snapshot_ids_.clear();
    return base::Result<void>::success();
}

void VssRequester::release_components() noexcept {
    if (components_ && writer_metadata_gathered_) {
        components_->FreeWriterMetadata();
    }
    writer_metadata_gathered_ = false;
    components_.reset();
}

enum class WorkerCommand {
    kWait,
    kClose,
    kAbandon,
};

struct WorkerControl final {
    std::mutex mutex;
    std::condition_variable condition;
    WorkerCommand command{WorkerCommand::kWait};
    std::promise<base::Result<void>> close_result;
};

base::Error unexpected_worker_error() {
    return base::Error{base::ErrorCode::kInternal, "VSS worker failed unexpectedly"};
}

void run_vss_worker(std::vector<WindowsVssSnapshotRequest> requests,
                    const base::CancellationToken& cancellation,
                    std::promise<base::Result<std::vector<WindowsVssSnapshot>>> create_result,
                    const std::shared_ptr<WorkerControl>& control) noexcept {
    bool creation_reported = false;
    try {
        auto apartment = ComApartment::enter();
        if (!apartment) {
            create_result.set_value(
                base::Result<std::vector<WindowsVssSnapshot>>::failure(apartment.error()));
            return;
        }
        VssRequester requester;
        auto created = requester.create(requests, cancellation);
        const bool creation_succeeded = created.has_value();
        create_result.set_value(std::move(created));
        creation_reported = true;
        if (!creation_succeeded) {
            return;
        }

        std::unique_lock lock(control->mutex);
        control->condition.wait(lock,
                                [&control] { return control->command != WorkerCommand::kWait; });
        const auto command = control->command;
        lock.unlock();
        if (command == WorkerCommand::kClose) {
            control->close_result.set_value(requester.close());
        } else {
            requester.abandon();
        }
    } catch (...) {
        try {
            if (!creation_reported) {
                create_result.set_value(base::Result<std::vector<WindowsVssSnapshot>>::failure(
                    unexpected_worker_error()));
            } else {
                control->close_result.set_value(
                    base::Result<void>::failure(unexpected_worker_error()));
            }
        } catch (...) {
            return;
        }
    }
}

class WindowsVssBackend final : public IVssSnapshotBackend {
  public:
    WindowsVssBackend() = default;
    ~WindowsVssBackend() override { abandon(); }

    WindowsVssBackend(const WindowsVssBackend&) = delete;
    WindowsVssBackend& operator=(const WindowsVssBackend&) = delete;
    WindowsVssBackend(WindowsVssBackend&&) = delete;
    WindowsVssBackend& operator=(WindowsVssBackend&&) = delete;

    [[nodiscard]] base::Result<std::vector<WindowsVssSnapshot>>
    create(std::span<const WindowsVssSnapshotRequest> requests,
           const base::CancellationToken& cancellation) override;
    [[nodiscard]] base::Result<void> close() override;
    void abandon() noexcept override;

  private:
    void signal(WorkerCommand command) noexcept;

    std::shared_ptr<WorkerControl> control_;
    std::future<base::Result<void>> close_result_;
    std::jthread worker_;
};

base::Result<std::vector<WindowsVssSnapshot>>
WindowsVssBackend::create(const std::span<const WindowsVssSnapshotRequest> requests,
                          const base::CancellationToken& cancellation) {
    control_ = std::make_shared<WorkerControl>();
    close_result_ = control_->close_result.get_future();
    std::promise<base::Result<std::vector<WindowsVssSnapshot>>> promise;
    auto future = promise.get_future();
    worker_ = std::jthread(run_vss_worker,
                           std::vector<WindowsVssSnapshotRequest>(requests.begin(), requests.end()),
                           cancellation, std::move(promise), control_);
    base::Result<std::vector<WindowsVssSnapshot>> result = [&future] {
        try {
            return future.get();
        } catch (...) {
            return base::Result<std::vector<WindowsVssSnapshot>>::failure(
                unexpected_worker_error());
        }
    }();
    if (!result) {
        worker_.join();
    }
    return result;
}

base::Result<void> WindowsVssBackend::close() {
    if (!worker_.joinable()) {
        return base::Result<void>::success();
    }
    signal(WorkerCommand::kClose);
    base::Result<void> result = base::Result<void>::success();
    try {
        result = close_result_.get();
    } catch (...) {
        result = base::Result<void>::failure(unexpected_worker_error());
    }
    worker_.join();
    return result;
}

void WindowsVssBackend::abandon() noexcept {
    if (!worker_.joinable()) {
        return;
    }
    signal(WorkerCommand::kAbandon);
    worker_.join();
}

void WindowsVssBackend::signal(const WorkerCommand command) noexcept {
    {
        const std::scoped_lock lock(control_->mutex);
        if (control_->command == WorkerCommand::kWait) {
            control_->command = command;
        }
    }
    control_->condition.notify_one();
}

} // namespace

std::unique_ptr<IVssSnapshotBackend> make_windows_vss_backend() {
    return std::make_unique<WindowsVssBackend>();
}

base::Result<bool>
probe_volume_snapshot_supported(const std::filesystem::path& volume_guid_path) {
    if (!is_canonical_volume_guid_path(volume_guid_path)) {
        return base::Result<bool>::failure(base::Error{
            base::ErrorCode::kInvalidArgument,
            "VSS volume path is not a canonical Volume GUID path",
        });
    }
    try {
        auto apartment = ComApartment::enter();
        if (!apartment) {
            return base::Result<bool>::failure(apartment.error());
        }
        auto security = initialize_com_security();
        if (!security) {
            return base::Result<bool>::failure(security.error());
        }
        ComPtr<IVssBackupComponents> components;
        auto result = CreateVssBackupComponents(components.put());
        if (FAILED(result)) {
            return base::Result<bool>::failure(
                hresult_error(result, "CreateVssBackupComponents"));
        }
        result = components->InitializeForBackup();
        if (FAILED(result)) {
            return base::Result<bool>::failure(hresult_error(result, "InitializeForBackup"));
        }
        result = components->SetContext(VSS_CTX_BACKUP);
        if (FAILED(result)) {
            return base::Result<bool>::failure(hresult_error(result, "SetContext"));
        }
        BOOL supported = FALSE;
        auto volume = volume_guid_path.native();
        result = components->IsVolumeSupported(GUID_NULL, volume.data(), &supported);
        if (FAILED(result)) {
            return base::Result<bool>::failure(hresult_error(result, "IsVolumeSupported"));
        }
        return base::Result<bool>::success(supported == TRUE);
    } catch (...) {
        return base::Result<bool>::failure(
            base::Error{base::ErrorCode::kInternal, "VSS support probe failed unexpectedly"});
    }
}

} // namespace aegra::adapters::windows_vss::detail
