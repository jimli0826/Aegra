#include "aegra/apps/boot_check/boot_check_host.h"

#include "aegra/adapters/hyperv/hyperv_scavenge.h"
#include "aegra/adapters/virtualbox/virtualbox_scavenge.h"
#include "aegra/apps/worker/worker_task_log.h"

#include <filesystem>
#include <string>
#include <system_error>

namespace aegra::apps::boot_check {
namespace {

using apps::worker::WorkerTaskLog;

[[nodiscard]] std::string path_utf8(const std::filesystem::path& path) {
    const auto encoded = path.u8string();
    return {reinterpret_cast<const char*>(encoded.data()), encoded.size()};
}

/// Powers off and unregisters the job's isolated VMs, then deletes the job
/// directory. Returns false when the directory could not be fully removed.
[[nodiscard]] bool scavenge_job_directory(const std::filesystem::path& job_directory,
                                          const BootCheckHostOptions& options,
                                          const BootCheckHostContext& context,
                                          WorkerTaskLog* const log) {
    const auto home = job_directory / "vbox-home";
    std::error_code probe;
    if (std::filesystem::is_directory(home, probe) && !options.vbox_manage_path.empty()) {
        auto removed = adapters::virtualbox::scavenge_boot_check_home(
            context.launcher, options.vbox_manage_path, path_utf8(home), {});
        if (log != nullptr) {
            log->field(path_utf8(job_directory.filename()),
                       removed ? "vms_unregistered=" + std::to_string(removed.value())
                               : "vm_scavenge_failed");
        }
    }
    std::error_code error;
    std::filesystem::remove_all(job_directory, error);
    return !error;
}

} // namespace

BootCheckExitCode run_boot_check_scavenge(const BootCheckHostOptions& options,
                                          const BootCheckHostContext& context,
                                          const std::filesystem::path& data_directory) {
    auto log = WorkerTaskLog::open("bootcheck", "scavenge");
    bool incomplete = false;
    std::error_code error;
    const auto jobs_directory = data_directory / "bootcheck" / "jobs";
    if (log != nullptr) {
        log->section("Scavenge");
        log->field("jobs_directory", path_utf8(jobs_directory));
    }
    if (std::filesystem::is_directory(jobs_directory, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(jobs_directory, error)) {
            if (error) {
                incomplete = true;
                break;
            }
            if (!entry.is_directory(error) || error) {
                continue;
            }
            if (!scavenge_job_directory(entry.path(), options, context, log.get())) {
                incomplete = true;
                if (log != nullptr) {
                    log->warn("job directory removal incomplete: " +
                              path_utf8(entry.path().filename()));
                }
            }
        }
    }
    // Hyper-V VMs live in the global inventory (no per-job registry); the sweep
    // is double-gated on the name prefix + creation Notes marker.
    if (!options.powershell_path.empty()) {
        auto removed = adapters::hyperv::scavenge_boot_check_vms(context.launcher,
                                                                 options.powershell_path, {});
        if (log != nullptr) {
            log->field("hyperv", removed ? "vms_removed=" + std::to_string(removed.value())
                                         : "vm_scavenge_failed");
        }
        if (!removed) {
            incomplete = true;
        }
    }
    const auto staging_directory = data_directory / "bootcheck" / "staging";
    if (std::filesystem::is_directory(staging_directory, error) && !error) {
        for (const auto& entry : std::filesystem::directory_iterator(staging_directory, error)) {
            if (error) {
                break;
            }
            std::error_code remove_error;
            std::filesystem::remove(entry.path(), remove_error);
        }
    }
    if (log != nullptr) {
        log->section("Result");
        log->field("outcome", incomplete ? "incomplete" : "clean");
    }
    return incomplete ? BootCheckExitCode::kTaskFailed : BootCheckExitCode::kSucceeded;
}

} // namespace aegra::apps::boot_check
