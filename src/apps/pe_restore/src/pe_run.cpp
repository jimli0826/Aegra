#include "pe_run.h"

#include "pe_worker_client.h"

#include "aegra/adapters/crypto_sodium/pe_envelope.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_pe/pe_pending_store.h"
#include "aegra/adapters/windows_pe/pe_session_log.h"
#include "aegra/adapters/windows_process/windows_process_launcher.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/contracts/pe_restore.h"
#include "aegra/ports/pe_pending_store.h"

#include <Windows.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::apps::pe_restore {
namespace {

inline constexpr std::uint32_t kMaximumProbedDisks = 64;
inline constexpr std::int64_t kWorkerDeadlineMs = 48LL * 60 * 60 * 1000;

struct FlowError final {
    const char* code{"pe_restore.failed"};
    base::Error error;
};

template <typename T>
using FlowResult = base::Result<T>;

[[nodiscard]] std::string trimmed(std::string value) {
    while (!value.empty() && (value.back() == ' ' || value.back() == '\0')) {
        value.pop_back();
    }
    std::size_t start = 0;
    while (start < value.size() && value[start] == ' ') {
        ++start;
    }
    return value.substr(start);
}

[[nodiscard]] std::string resolve_layer_path(const contracts::PeChainLayer& layer) {
    // `\\?\Volume{...}\` + volume-relative path; forward slashes normalized.
    std::string path = layer.volume_guid;
    std::string relative = layer.relative_path;
    for (char& character : relative) {
        if (character == '/') {
            character = '\\';
        }
    }
    path += relative;
    return path;
}

[[nodiscard]] bool file_exists_utf8(const std::string& path_utf8) {
    const auto required = MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return false;
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, path_utf8.c_str(), -1, wide.data(), required) <= 0) {
        return false;
    }
    wide.resize(static_cast<std::size_t>(required) - 1);
    return GetFileAttributesW(wide.c_str()) != INVALID_FILE_ATTRIBUTES;
}

/// Serial-first identity re-match (design §8.2). Exactly one disk must match the
/// recorded serial and size; anything else refuses the restore.
[[nodiscard]] FlowResult<std::uint32_t>
match_target_disk(const contracts::PeTargetDiskIdentity& target) {
    const std::string wanted_serial = trimmed(target.serial_number);
    std::vector<std::uint32_t> matches;
    for (std::uint32_t disk = 0; disk < kMaximumProbedDisks; ++disk) {
        auto layout = adapters::windows_disk::inspect_physical_disk_layout(disk);
        if (!layout) {
            continue;
        }
        if (trimmed(layout.value().serial) == wanted_serial &&
            layout.value().disk_size_bytes == target.size_bytes) {
            matches.push_back(disk);
        }
    }
    if (matches.size() != 1) {
        return FlowResult<std::uint32_t>::failure(
            {base::ErrorCode::kConflict,
             matches.empty() ? "no disk matches the recorded target identity"
                             : "multiple disks match the recorded target identity"});
    }
    return FlowResult<std::uint32_t>::success(matches.front());
}

struct OpenedSecret final {
    /// Plaintext password bytes; zeroized by the destructor.
    std::vector<std::byte> bytes;

    ~OpenedSecret() {
        if (!bytes.empty()) {
            SecureZeroMemory(bytes.data(), bytes.size());
        }
    }
    OpenedSecret() = default;
    OpenedSecret(const OpenedSecret&) = delete;
    OpenedSecret& operator=(const OpenedSecret&) = delete;
    OpenedSecret(OpenedSecret&&) = default;
    OpenedSecret& operator=(OpenedSecret&&) = default;

    [[nodiscard]] std::string_view view() const noexcept {
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) UTF-8 view of bytes.
        return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
    }
};

struct FlowState final {
    std::unique_ptr<ports::IPePendingJobStore> store;
    std::unique_ptr<adapters::windows_pe::PeSessionLog> session_log;
    contracts::PePendingJobV1 job;
    std::uint32_t target_disk_number{0};
    std::vector<std::string> source_paths;
    adapters::windows_system::WindowsSystemClock clock;
    adapters::windows_system::WindowsCryptographicRandom random;
    adapters::windows_process::WindowsProcessLauncher launcher;
};

[[nodiscard]] std::string summary_backup_display(const contracts::PePendingJobV1& job) {
    const auto& tip = job.source.chain.back();
    return tip.online_path.empty() ? tip.relative_path : tip.online_path;
}

[[nodiscard]] std::string summary_target_display(const contracts::PeTargetDiskIdentity& target) {
    std::string display = target.friendly_name.empty() ? "disk" : target.friendly_name;
    display += "  SN:";
    display += target.serial_number;
    display += "  ";
    display += std::to_string(target.size_bytes / (1024ULL * 1024ULL * 1024ULL));
    display += " GB";
    return display;
}

[[nodiscard]] PeRunSummary make_summary(const contracts::PePendingJobV1& job) {
    PeRunSummary summary;
    summary.locale = job.ui.locale;
    summary.backup_display = summary_backup_display(job);
    summary.target_display = summary_target_display(job.target);
    summary.auto_start_seconds = job.ui.auto_start_seconds;
    summary.needs_password = job.envelope.mode == contracts::PeEnvelopeMode::kPrompt;
    return summary;
}

/// Sealed mode: recompute the binding, open the envelope, then burn the key file
/// (use-once, before anything destructive; ADR-0026 B4).
[[nodiscard]] FlowResult<OpenedSecret> open_sealed_password(FlowState& state) {
    auto key = state.store->read_job_key({});
    if (!key) {
        return FlowResult<OpenedSecret>::failure(key.error());
    }
    const auto binding = contracts::pe_pending_job_binding(state.job);
    auto opened = adapters::crypto_sodium::open_pe_password(
        state.job.envelope.nonce, state.job.envelope.ciphertext_with_tag, key.value(), binding);
    SecureZeroMemory(key.value().data(), key.value().size());
    if (!opened) {
        return FlowResult<OpenedSecret>::failure(opened.error());
    }
    if (auto burned = state.store->consume_job_key(); !burned) {
        SecureZeroMemory(opened.value().data(), opened.value().size());
        return FlowResult<OpenedSecret>::failure(burned.error());
    }
    OpenedSecret secret;
    secret.bytes = std::move(opened).value();
    return FlowResult<OpenedSecret>::success(std::move(secret));
}

[[nodiscard]] FlowResult<contracts::JobRequest> build_worker_job(FlowState& state,
                                                                 const std::string_view password) {
    contracts::JobRequest job;
    job.job_id = state.job.job_uuid;
    job.tenant_id = "personal";
    job.operation = contracts::JobOperation::kRestore;
    job.content_kind = contracts::ContentKind::kVolumeSet;
    job.source_refs = state.source_paths;
    job.target_ref = std::string(R"(\\.\PhysicalDrive)") +
                     std::to_string(state.target_disk_number);
    job.trace_id = "pe-" + state.job.job_uuid;
    job.deadline_utc_ms = state.clock.now_utc_ms() + kWorkerDeadlineMs;
    if (password.empty()) {
        job.credential_refs.assign(job.source_refs.size(), contracts::SecretRef{});
    } else {
        // DPAPI machine scope protects and unprotects within this same PE boot
        // session; the worker's dpapi-lm resolver works unchanged. The entropy id
        // grammar is lowercase-only.
        std::string entropy_id = state.job.job_uuid;
        for (char& character : entropy_id) {
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        }
        auto secret_ref =
            adapters::windows_system::protect_local_machine_secret(password, entropy_id);
        if (!secret_ref) {
            return FlowResult<contracts::JobRequest>::failure(secret_ref.error());
        }
        job.credential_refs.assign(job.source_refs.size(), secret_ref.value());
    }
    contracts::RestoreOptions restore;
    restore.disk_restore = true;
    restore.source_disk_number = state.job.source.source_disk_number;
    // No mounted volumes and no drive letters inside PE (design §9): stay offline.
    restore.bring_target_online = false;
    restore.preserve_disk_signature = state.job.options.preserve_disk_signature;
    restore.auto_expand_last_partition = state.job.options.auto_expand_last_partition;
    restore.volume_size_policy = contracts::VolumeSizePolicy::kRequireSourceSize;
    job.restore = std::move(restore);
    return FlowResult<contracts::JobRequest>::success(std::move(job));
}

[[nodiscard]] std::string worker_executable_beside_self() {
    wchar_t module_path[MAX_PATH]{};
    const auto length = GetModuleFileNameW(nullptr, module_path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    std::wstring path(module_path, length);
    const auto separator = path.find_last_of(L'\\');
    if (separator == std::wstring::npos) {
        return {};
    }
    path.resize(separator + 1);
    path += L"aegra_personal_worker.exe";
    const auto required = WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, nullptr, 0, nullptr,
                                              nullptr);
    if (required <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8.data(), required, nullptr,
                            nullptr) <= 0) {
        return {};
    }
    utf8.resize(static_cast<std::size_t>(required) - 1);
    return utf8;
}

/// PE RAM-disk root ("X:\"), derived from the PE system directory rather than
/// assuming X:. The worker's data dir MUST live here, never on the restore target:
/// dismounting the target's volumes (required for the raw rewrite) would otherwise
/// hang on / invalidate the worker's own open log handle when the target is the
/// system disk (its ProgramData volume holds the log).
[[nodiscard]] std::wstring pe_ram_root() {
    wchar_t windows_dir[MAX_PATH]{};
    const auto length = GetSystemWindowsDirectoryW(windows_dir, MAX_PATH);
    if (length >= 3 && windows_dir[1] == L':' && windows_dir[2] == L'\\') {
        return std::wstring(windows_dir, 3);
    }
    return L"X:\\";
}

/// Publishes AEGRA_DATA_DIR (RAM disk) and AEGRA_WINPE for the spawned worker. The
/// per-task log lands on the RAM disk; the executor mirrors it to a persistent
/// non-target volume after the worker exits.
void set_worker_environment() {
    const std::wstring data_dir = pe_ram_root() + L"Aegra";
    SetEnvironmentVariableW(L"AEGRA_DATA_DIR", data_dir.c_str());
    // Authoritative offline-environment signal for the worker's disk adapter: a
    // WinRE-derived image may lack the registry MiniNT marker that auto-detection
    // relies on, so the disk-offline tolerance would otherwise not engage.
    SetEnvironmentVariableW(L"AEGRA_WINPE", L"1");
}

[[nodiscard]] std::wstring wide_from_utf8(const std::string& value) {
    if (value.empty()) {
        return {};
    }
    const auto required = MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, wide.data(), required) <= 0) {
        return {};
    }
    wide.resize(static_cast<std::size_t>(required) - 1);
    return wide;
}

/// `\\?\Volume{...}\` prefix of an archive path — a volume that preflight guarantees
/// is NOT the restore target, so a mirrored log persists across the reboot.
[[nodiscard]] std::wstring volume_root_of(const std::string& archive_path_utf8) {
    const std::wstring wide = wide_from_utf8(archive_path_utf8);
    constexpr std::wstring_view prefix = LR"(\\?\Volume{)";
    if (!wide.starts_with(prefix)) {
        return {};
    }
    const auto brace = wide.find(L'}');
    if (brace == std::wstring::npos || brace + 1 >= wide.size() || wide[brace + 1] != L'\\') {
        return {};
    }
    return wide.substr(0, brace + 2); // include "}\"
}

/// Copies the newest worker restore log from the RAM disk to a persistent non-target
/// volume so a failed system-disk restore is still diagnosable after reboot.
/// Best-effort: never affects the restore outcome.
void mirror_worker_log(const std::wstring& persistent_volume_root) {
    if (persistent_volume_root.empty()) {
        return;
    }
    const std::wstring source_dir = pe_ram_root() + LR"(Aegra\logs\restore)";
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((source_dir + LR"(\*.log)").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE) {
        return;
    }
    std::wstring newest_name;
    FILETIME newest_time{};
    do {
        if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (CompareFileTime(&found.ftLastWriteTime, &newest_time) >= 0) {
            newest_time = found.ftLastWriteTime;
            newest_name = found.cFileName;
        }
    } while (FindNextFileW(find, &found));
    FindClose(find);
    if (newest_name.empty()) {
        return;
    }
    const std::wstring destination_dir = persistent_volume_root + L"AegraPeRestoreLogs";
    CreateDirectoryW(destination_dir.c_str(), nullptr);
    CopyFileW((source_dir + L'\\' + newest_name).c_str(),
              (destination_dir + L'\\' + newest_name).c_str(), FALSE);
}

[[nodiscard]] PeRunOutcome failed_outcome(const std::string& locale, const char* code,
                                          const base::Error& error) {
    PeRunOutcome outcome;
    outcome.kind = PeRunResultKind::kFailed;
    outcome.error_code = code;
    outcome.error_message = error.message;
    outcome.locale = locale;
    outcome.reboot = false;
    return outcome;
}

void persist_result(FlowState& state, const PeRunOutcome& outcome) {
    if (!state.store) {
        return;
    }
    contracts::PeRestoreResultV1 result;
    result.job_uuid = state.job.job_uuid.empty() ? "unknown" : state.job.job_uuid;
    result.finished_utc_ms = state.clock.now_utc_ms();
    switch (outcome.kind) {
    case PeRunResultKind::kSuccess:
        result.status = contracts::PeRestoreStatus::kSuccess;
        break;
    case PeRunResultKind::kCancelled:
        result.status = contracts::PeRestoreStatus::kCancelled;
        break;
    case PeRunResultKind::kFailed:
        result.status = contracts::PeRestoreStatus::kFailed;
        break;
    }
    result.error_code = outcome.error_code;
    result.error_message = outcome.error_message;
    (void)state.store->write_result(result, {});
}

[[nodiscard]] PeRunOutcome cancelled_outcome(FlowState& state) {
    PeRunOutcome outcome;
    outcome.kind = PeRunResultKind::kCancelled;
    outcome.error_code = "pe_restore.cancelled";
    outcome.locale = state.job.ui.locale;
    outcome.reboot = true;
    // Nothing was written; burn the key so the pending secret cannot be replayed.
    (void)state.store->consume_job_key();
    return outcome;
}

[[nodiscard]] FlowResult<PeWorkerOutcome> dispatch_worker(FlowState& state, IPeRunView& view,
                                                          const std::string_view password) {
    auto job = build_worker_job(state, password);
    if (!job) {
        return FlowResult<PeWorkerOutcome>::failure(job.error());
    }
    PeWorkerSessionRequest session;
    session.job = std::move(job).value();
    session.worker_executable_path_utf8 = worker_executable_beside_self();
    session.launcher = &state.launcher;
    session.random = &state.random;
    if (state.session_log) {
        state.session_log->line("dispatching worker: " + session.worker_executable_path_utf8 +
                                "; target=" + session.job.target_ref);
    }
    auto* session_log = state.session_log.get();
    auto last_progress_code = std::make_shared<std::string>();
    session.on_progress = [&view, session_log,
                           last_progress_code](const PeWorkerProgress& progress) {
        if (session_log != nullptr && progress.message_code != *last_progress_code) {
            *last_progress_code = progress.message_code;
            session_log->line("worker progress: " + progress.message_code);
        }
        PeRunProgress view_progress;
        view_progress.written_bytes = progress.processed_bytes;
        if (progress.logical_bytes.has_value() && *progress.logical_bytes > 0) {
            view_progress.has_percent = true;
            const auto percent = (progress.processed_bytes * 100U) / *progress.logical_bytes;
            view_progress.percent = static_cast<std::uint32_t>((std::min)(percent,
                                                                          std::uint64_t{99}));
        }
        view.progress_changed(view_progress);
    };
    auto outcome = run_pe_worker_session(session, {});
    if (state.session_log) {
        if (outcome) {
            state.session_log->line(
                "worker finished: outcome=" +
                std::to_string(static_cast<int>(outcome.value().outcome)) +
                "; error=" + std::string(base::error_code_name(outcome.value().error_code)) +
                "; message_code=" + outcome.value().message_code);
        } else {
            state.session_log->line("worker session failed: " + outcome.error().message);
        }
    }
    return outcome;
}

[[nodiscard]] PeRunOutcome run_flow_steps(FlowState& state, IPeRunView& view) {
    view.stage_changed(PeRunStage::kLocatingJob);
    auto located = adapters::windows_pe::locate_pe_pending_store();
    if (!located) {
        return failed_outcome("en-US", "pe_restore.job_not_found", located.error());
    }
    state.store = std::move(located.value().store);
    // The worker logs to the RAM disk (never the restore target); the executor
    // mirrors that log to the archive volume after the worker exits. Writing to the
    // target would hang on the worker's own log handle when the target is the
    // system disk (its volumes get dismounted for the raw rewrite).
    set_worker_environment();
    state.session_log = adapters::windows_pe::PeSessionLog::open(located.value().data_dir_utf8);
    if (state.session_log) {
        state.session_log->line("pe executor session started; version=" AEGRA_PE_VERSION);
        state.session_log->line("host data dir: " + located.value().data_dir_utf8);
    }
    auto job = state.store->read_pending({});
    if (!job) {
        return failed_outcome("en-US", "pe_restore.job_invalid", job.error());
    }
    state.job = std::move(job).value();
    const auto& locale = state.job.ui.locale;
    if (state.session_log) {
        state.session_log->line("job loaded: " + state.job.job_uuid +
                                "; layers=" + std::to_string(state.job.source.chain.size()) +
                                "; target_serial=" + state.job.target.serial_number +
                                "; target_size=" + std::to_string(state.job.target.size_bytes));
    }

    view.stage_changed(PeRunStage::kVerifying);
    for (const auto& layer : state.job.source.chain) {
        auto path = resolve_layer_path(layer);
        if (!file_exists_utf8(path)) {
            return failed_outcome(locale, "pe_restore.chain_unreachable",
                                  {base::ErrorCode::kNotFound,
                                   "backup chain layer is unreachable: " + layer.relative_path});
        }
        if (state.session_log) {
            state.session_log->line("chain layer ok: " + path);
        }
        state.source_paths.push_back(std::move(path));
    }
    auto matched = match_target_disk(state.job.target);
    if (!matched) {
        return failed_outcome(locale, "pe_restore.target_mismatch", matched.error());
    }
    state.target_disk_number = matched.value();
    if (state.session_log) {
        state.session_log->line("target matched: PhysicalDrive" +
                                std::to_string(state.target_disk_number));
    }

    if (view.wait_start_decision(make_summary(state.job)) == PeUserDecision::kCancel) {
        return cancelled_outcome(state);
    }

    OpenedSecret secret;
    if (state.job.envelope.mode == contracts::PeEnvelopeMode::kSealed) {
        auto opened = open_sealed_password(state);
        if (!opened) {
            return failed_outcome(locale, "pe_restore.envelope_failed", opened.error());
        }
        secret = std::move(opened).value();
    } else if (state.job.envelope.mode == contracts::PeEnvelopeMode::kPrompt) {
        auto entered = view.wait_password();
        if (!entered) {
            return entered.error().code == base::ErrorCode::kCancelled
                       ? cancelled_outcome(state)
                       : failed_outcome(locale, "pe_restore.envelope_failed", entered.error());
        }
        secret.bytes.resize(entered.value().size());
        std::memcpy(secret.bytes.data(), entered.value().data(), entered.value().size());
        SecureZeroMemory(entered.value().data(), entered.value().size());
    }

    view.stage_changed(PeRunStage::kRestoring);
    auto outcome = dispatch_worker(state, view, secret.view());
    // Persist the worker's RAM-disk log to the archive volume (non-target, survives
    // reboot) so a failure is diagnosable regardless of the outcome. Best-effort.
    if (!state.source_paths.empty()) {
        mirror_worker_log(volume_root_of(state.source_paths.front()));
    }
    if (!outcome) {
        return failed_outcome(locale, "pe_restore.worker_failed", outcome.error());
    }
    if (outcome.value().outcome == contracts::TaskOutcome::kSucceeded ||
        outcome.value().outcome == contracts::TaskOutcome::kSucceededWithWarning) {
        PeRunOutcome success;
        success.kind = PeRunResultKind::kSuccess;
        success.locale = locale;
        success.reboot = true;
        return success;
    }
    if (outcome.value().outcome == contracts::TaskOutcome::kCancelled) {
        return cancelled_outcome(state);
    }
    return failed_outcome(locale,
                          outcome.value().message_code.empty() ? "pe_restore.worker_failed"
                                                               : "pe_restore.worker_failed",
                          {outcome.value().error_code, outcome.value().message_code});
}

} // namespace

PeRunOutcome run_pe_restore_flow(IPeRunView& view) {
    FlowState state;
    auto outcome = run_flow_steps(state, view);
    persist_result(state, outcome);
    if (state.session_log) {
        state.session_log->line(
            "session outcome: kind=" + std::to_string(static_cast<int>(outcome.kind)) +
            "; code=" + outcome.error_code + "; message=" + outcome.error_message);
    }
    return outcome;
}

void reboot_winpe() {
    // wpeutil is present in every WinRE/WinPE image; best-effort by design.
    wchar_t system_directory[MAX_PATH]{};
    if (GetSystemDirectoryW(system_directory, MAX_PATH) == 0) {
        return;
    }
    std::wstring command = L"\"";
    command += system_directory;
    command += L"\\wpeutil.exe\" reboot";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
                       nullptr, &startup, &process)) {
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
}

} // namespace aegra::apps::pe_restore
