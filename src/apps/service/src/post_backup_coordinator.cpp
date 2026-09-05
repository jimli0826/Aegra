#include "aegra/apps/service/post_backup_coordinator.h"

#include "aegra/apps/service/boot_check_supervisor.h"
#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/worker_job_service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <stop_token>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

constexpr auto kScanInterval = std::chrono::seconds(15);
constexpr std::uint64_t kLeaseMilliseconds = 120'000;
constexpr std::uint32_t kMaximumClaimedPlans = 8;
constexpr std::uint32_t kMaximumSubmitAttempts = 10;

constexpr std::uint32_t kMaximumBootCheckAttempts = 3;

constexpr const char* kVerifySkippedPrerequisite = "post_backup.verify_prerequisite_failed";
constexpr const char* kVerifySubmitFailed = "post_backup.verify_submit_failed";
constexpr const char* kVerifyJobMissing = "post_backup.verify_job_missing";
constexpr const char* kBootCheckUnavailable = "post_backup.boot_check_unavailable";
constexpr const char* kBootCheckInterrupted = "post_backup.boot_check_interrupted";

void write_log(IServiceLog* const logger, const ServiceLogLevel level, const std::string_view code,
               const std::string_view detail) noexcept {
    if (logger != nullptr) {
        logger->write(level, code, detail);
    }
}

[[nodiscard]] bool is_capacity_conflict(const base::Error& error) noexcept {
    return error.code == base::ErrorCode::kConflict &&
           error.message.find("worker capacity") != std::string::npos;
}

[[nodiscard]] std::string random_run_suffix(ports::IRandomSource& random) {
    std::array<std::byte, 4> bytes{};
    std::string suffix;
    if (!random.fill(bytes, {})) {
        return suffix;
    }
    constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        suffix.push_back(kHex[value >> 4U]);
        suffix.push_back(kHex[value & 0x0FU]);
    }
    return suffix;
}

[[nodiscard]] std::string random_owner_id(ports::IRandomSource& random) {
    std::array<std::byte, 8> bytes{};
    std::string owner = "post-backup-coordinator-";
    if (!random.fill(bytes, {})) {
        return owner + "0";
    }
    constexpr char kHex[] = "0123456789abcdef";
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        owner.push_back(kHex[value >> 4U]);
        owner.push_back(kHex[value & 0x0FU]);
    }
    return owner;
}

} // namespace

struct PostBackupCoordinator::Impl final {
    Impl(ports::IControlPlaneDatabase& database, ports::IClock& clock_source,
         ports::IRandomSource& random_source, IServiceLog* const service_logger)
        : control_plane(database), clock(clock_source), random(random_source),
          logger(service_logger) {}

    ports::IControlPlaneDatabase& control_plane;
    ports::IClock& clock;
    ports::IRandomSource& random;
    IServiceLog* logger{nullptr};
    WorkerJobService* worker_jobs{nullptr};
    BootCheckSupervisor* boot_check{nullptr};
    std::string claim_owner;
    std::mutex mutex;
    std::condition_variable_any changed;
    bool kicked{false};
    bool stopping{false};
    std::jthread scanner;

    // begin_unit_of_work fails fast with kConflict when a reader (e.g. the
    // Desktop polling ListJobs) momentarily holds the single-connection lock.
    // Coordinator writes must not be dropped on that race, so retry briefly.
    [[nodiscard]] base::Result<std::unique_ptr<ports::IControlPlaneUnitOfWork>> begin_unit_retry() {
        constexpr int kMaximumAttempts = 40;
        constexpr auto kRetryDelay = std::chrono::milliseconds(10);
        for (int attempt = 0;; ++attempt) {
            auto unit = control_plane.begin_unit_of_work({});
            if (unit || unit.error().code != base::ErrorCode::kConflict ||
                attempt >= kMaximumAttempts) {
                return unit;
            }
            std::this_thread::sleep_for(kRetryDelay);
        }
    }

    [[nodiscard]] std::uint64_t now_utc_ms() const {
        const auto now = clock.now_utc_ms();
        return now < 0 ? 0 : static_cast<std::uint64_t>(now);
    }

    void log_action(const ServiceLogLevel level, const char* const code,
                    const ports::PostBackupPlanRecord& plan, const std::string_view detail) const {
        std::string message = "Post-backup plan for backup job ";
        message += plan.backup_job_id;
        message += ": ";
        message += detail;
        write_log(logger, level, code, message);
    }

    // Submits the planned Verify. Pending is kept (capacity/catalog races retry on
    // the next scan); a submit rejection becomes a terminal failed action.
    void drive_pending_verify(ports::PostBackupPlanRecord& plan) {
        PostBackupVerifyRequest request;
        request.backup_job_id = plan.backup_job_id;
        request.recovery_point_id = plan.recovery_point_id;
        request.repository_connection_id = plan.repository_connection_id;
        request.schedule_id = plan.schedule_id;
        auto submitted = worker_jobs->start_post_backup_verify(request, {});
        if (submitted) {
            plan.verify.state = ports::PostBackupActionState::kRunning;
            plan.verify.child_job_id = submitted.value().resource_id;
            plan.verify.message_code = "post_backup.verify_submitted";
            plan.verify.attempts += 1;
            log_action(ServiceLogLevel::kInfo, "post_backup.verify_submitted", plan,
                       "verify_job=" + submitted.value().resource_id.value_or(""));
            return;
        }
        if (is_capacity_conflict(submitted.error())) {
            return;
        }
        if (submitted.error().code == base::ErrorCode::kNotFound &&
            plan.verify.attempts + 1 < kMaximumSubmitAttempts) {
            // Catalog publish may still be settling right after completion.
            plan.verify.attempts += 1;
            return;
        }
        plan.verify.state = ports::PostBackupActionState::kFailed;
        plan.verify.message_code = kVerifySubmitFailed;
        plan.verify.attempts += 1;
        std::string detail = "submit failed; error=";
        detail += base::error_code_name(submitted.error().code);
        if (!submitted.error().message.empty()) {
            detail += "; detail=" + submitted.error().message;
        }
        log_action(ServiceLogLevel::kError, kVerifySubmitFailed, plan, detail);
    }

    void drive_running_verify(ports::PostBackupPlanRecord& plan) {
        auto child = control_plane.get_job(plan.verify.child_job_id.value_or(""), {});
        if (!child) {
            return;
        }
        if (!child.value()) {
            plan.verify.state = ports::PostBackupActionState::kFailed;
            plan.verify.message_code = kVerifyJobMissing;
            log_action(ServiceLogLevel::kError, kVerifyJobMissing, plan,
                       "verify job record disappeared");
            return;
        }
        if (!ports::is_terminal_job_state(child.value()->state)) {
            return;
        }
        const bool succeeded = child.value()->state == contracts::ServiceJobState::kSucceeded;
        plan.verify.state = succeeded ? ports::PostBackupActionState::kSucceeded
                                      : ports::PostBackupActionState::kFailed;
        plan.verify.message_code =
            child.value()->result_message_code.value_or(child.value()->message_code);
        log_action(succeeded ? ServiceLogLevel::kInfo : ServiceLogLevel::kError,
                   succeeded ? "post_backup.verify_succeeded" : "post_backup.verify_failed", plan,
                   "verify_job=" + plan.verify.child_job_id.value_or("") +
                       " message=" + plan.verify.message_code);
    }

    void skip_remaining_actions(ports::PostBackupPlanRecord& plan, const char* const message_code) {
        if (plan.verify_required &&
            !ports::is_terminal_post_backup_action_state(plan.verify.state)) {
            plan.verify.state = ports::PostBackupActionState::kSkipped;
            plan.verify.message_code = message_code;
        }
        if (plan.boot_check_required &&
            !ports::is_terminal_post_backup_action_state(plan.boot_check.state)) {
            plan.boot_check.state = ports::PostBackupActionState::kSkipped;
            plan.boot_check.message_code = message_code;
        }
        log_action(ServiceLogLevel::kWarning, message_code, plan, "remaining actions skipped");
    }

    // Advances one claimed plan by at most one step per scan.
    void drive_plan(ports::PostBackupPlanRecord& plan) {
        auto backup = control_plane.get_job(plan.backup_job_id, {});
        if (!backup) {
            return;
        }
        if (!backup.value()) {
            skip_remaining_actions(plan, "post_backup.backup_job_missing");
            return;
        }
        if (!ports::is_terminal_job_state(backup.value()->state)) {
            return;
        }
        if (backup.value()->state != contracts::ServiceJobState::kSucceeded) {
            skip_remaining_actions(plan, kVerifySkippedPrerequisite);
            return;
        }
        if (plan.verify_required) {
            if (plan.verify.state == ports::PostBackupActionState::kPending) {
                drive_pending_verify(plan);
            } else if (plan.verify.state == ports::PostBackupActionState::kRunning) {
                drive_running_verify(plan);
            }
            if (plan.verify.state == ports::PostBackupActionState::kFailed &&
                plan.boot_check_required &&
                !ports::is_terminal_post_backup_action_state(plan.boot_check.state)) {
                plan.boot_check.state = ports::PostBackupActionState::kSkipped;
                plan.boot_check.message_code = "bootcheck.verify_prerequisite_failed";
                log_action(ServiceLogLevel::kWarning, "bootcheck.verify_prerequisite_failed", plan,
                           "boot check skipped because verify failed");
            }
        }
        const bool verify_satisfied =
            !plan.verify_required || plan.verify.state == ports::PostBackupActionState::kSucceeded;
        if (plan.boot_check_required && verify_satisfied &&
            !ports::is_terminal_post_backup_action_state(plan.boot_check.state)) {
            drive_boot_check(plan);
        }
    }

    // Boot check runs are recorded as control-plane jobs so the task log shows
    // them alongside backup and verify. Record failures never block the run;
    // stale Running rows are swept to Interrupted at Service startup.
    void record_boot_check_job_started(const ports::PostBackupPlanRecord& plan,
                                       const std::string& job_id) {
        ports::JobRecord record;
        record.job_id = job_id;
        record.trace_id = "trace-" + job_id;
        record.operation = contracts::JobOperation::kBootCheck;
        record.state = contracts::ServiceJobState::kRunning;
        record.content_kind = contracts::ContentKind::kVolumeSet;
        record.created_utc_ms = now_utc_ms();
        record.started_utc_ms = record.created_utc_ms;
        // Use the owning schedule's volume source ids so the task log shows the
        // same friendly volume names as the backup, not the raw recovery-point
        // uuid. Fall back to the recovery point id if the schedule is gone.
        if (auto schedule = control_plane.get_schedule(plan.schedule_id, {});
            schedule && schedule.value() && !schedule.value()->source_ids.empty()) {
            record.source_ids = schedule.value()->source_ids;
        } else {
            record.source_ids.push_back(plan.recovery_point_id);
        }
        record.schedule_id = plan.schedule_id;
        record.repository_connection_id = plan.repository_connection_id;
        record.message_code = "job.running";
        auto unit = begin_unit_retry();
        if (!unit) {
            return;
        }
        auto inserted = unit.value()->jobs().insert(record, {});
        if (!inserted) {
            unit.value()->rollback();
            log_action(ServiceLogLevel::kWarning, "post_backup.boot_check_job_record_failed", plan,
                       "boot check job insert failed; error=" +
                           std::string(base::error_code_name(inserted.error().code)));
            return;
        }
        (void)unit.value()->commit({});
    }

    void record_boot_check_job_terminal(const ports::PostBackupPlanRecord& plan,
                                        const std::string& job_id, const bool succeeded,
                                        const std::string& result_message_code) {
        ports::JobStateTransition transition;
        transition.job_id = job_id;
        transition.expected_state = contracts::ServiceJobState::kRunning;
        transition.next_state = succeeded ? contracts::ServiceJobState::kSucceeded
                                          : contracts::ServiceJobState::kFailed;
        transition.transition_utc_ms = now_utc_ms();
        transition.message_code = succeeded ? "job.succeeded" : "job.failed";
        if (!result_message_code.empty()) {
            transition.result_message_code = result_message_code;
        }
        auto unit = begin_unit_retry();
        if (!unit) {
            return;
        }
        auto stored = unit.value()->jobs().transition(transition, {});
        if (!stored) {
            unit.value()->rollback();
            log_action(ServiceLogLevel::kWarning, "post_backup.boot_check_job_record_failed", plan,
                       "boot check job transition failed; job=" + job_id + " error=" +
                           std::string(base::error_code_name(stored.error().code)));
            return;
        }
        (void)unit.value()->commit({});
    }

    void drive_boot_check(ports::PostBackupPlanRecord& plan) {
        if (boot_check == nullptr || !boot_check->available()) {
            plan.boot_check.state = ports::PostBackupActionState::kSkipped;
            plan.boot_check.message_code = kBootCheckUnavailable;
            log_action(ServiceLogLevel::kWarning, kBootCheckUnavailable, plan,
                       "boot check host is unavailable");
            return;
        }
        if (plan.boot_check.state == ports::PostBackupActionState::kPending) {
            if (plan.boot_check.attempts >= kMaximumBootCheckAttempts) {
                plan.boot_check.state = ports::PostBackupActionState::kFailed;
                plan.boot_check.message_code = kBootCheckInterrupted;
                log_action(ServiceLogLevel::kError, kBootCheckInterrupted, plan,
                           "boot check attempts exhausted");
                return;
            }
            BootCheckDispatch dispatch;
            // Attempt-scoped, session-unique id: an orphaned run from a previous
            // Service instance keeps its own VM/job-dir identity out of the
            // retry's way, and the run's job record never collides with one a
            // crash left behind.
            const auto suffix = random_run_suffix(random);
            dispatch.boot_check_job_id = "bootcheck-" + plan.recovery_point_id + "-a" +
                                         std::to_string(plan.boot_check.attempts + 1) +
                                         (suffix.empty() ? "" : "-" + suffix);
            dispatch.recovery_point_id = plan.recovery_point_id;
            dispatch.repository_connection_id = plan.repository_connection_id;
            dispatch.schedule_id = plan.schedule_id;
            dispatch.hypervisor = *plan.boot_check_hypervisor;
            if (boot_check->try_start(dispatch)) {
                plan.boot_check.state = ports::PostBackupActionState::kRunning;
                plan.boot_check.child_job_id = dispatch.boot_check_job_id;
                plan.boot_check.message_code = "post_backup.boot_check_submitted";
                plan.boot_check.attempts += 1;
                record_boot_check_job_started(plan, dispatch.boot_check_job_id);
                log_action(ServiceLogLevel::kInfo, "post_backup.boot_check_submitted", plan,
                           "boot_check_job=" + dispatch.boot_check_job_id);
            }
            return;
        }
        // Running: consume the supervisor result, or re-dispatch when a restart
        // lost the in-memory run (the durable plan keeps the attempt count).
        const auto child = plan.boot_check.child_job_id.value_or("");
        if (auto result = boot_check->take_result(child)) {
            plan.boot_check.state = result->succeeded ? ports::PostBackupActionState::kSucceeded
                                                      : ports::PostBackupActionState::kFailed;
            plan.boot_check.message_code = result->message_code;
            record_boot_check_job_terminal(plan, child, result->succeeded, result->message_code);
            log_action(result->succeeded ? ServiceLogLevel::kInfo : ServiceLogLevel::kError,
                       result->succeeded ? "post_backup.boot_check_succeeded"
                                         : "post_backup.boot_check_failed",
                       plan, "boot_check_job=" + child + " message=" + result->message_code);
            return;
        }
        if (!boot_check->is_tracking(child)) {
            plan.boot_check.state = ports::PostBackupActionState::kPending;
        }
    }

    void persist_plan(ports::PostBackupPlanRecord& plan) {
        plan.updated_utc_ms = (std::max)(now_utc_ms(), plan.updated_utc_ms);
        if (ports::is_complete_post_backup_plan(plan)) {
            plan.claim_owner.reset();
            plan.lease_expires_utc_ms.reset();
        }
        auto unit = begin_unit_retry();
        if (!unit) {
            write_log(logger, ServiceLogLevel::kError, "post_backup.plan_persist_failed",
                      "Post-backup plan begin failed for backup job " + plan.backup_job_id +
                          "; error=" + std::string(base::error_code_name(unit.error().code)));
            return;
        }
        auto stored = unit.value()->post_backup_plans().upsert(plan, {});
        if (!stored) {
            unit.value()->rollback();
            write_log(logger, ServiceLogLevel::kError, "post_backup.plan_persist_failed",
                      "Post-backup plan update failed for backup job " + plan.backup_job_id +
                          "; error=" + std::string(base::error_code_name(stored.error().code)));
            return;
        }
        if (auto committed = unit.value()->commit({}); !committed) {
            write_log(logger, ServiceLogLevel::kError, "post_backup.plan_persist_failed",
                      "Post-backup plan commit failed for backup job " + plan.backup_job_id +
                          "; error=" + std::string(base::error_code_name(committed.error().code)));
        }
    }

    void scan_once() {
        ports::PostBackupPlanClaimRequest claim;
        claim.claim_owner = claim_owner;
        claim.now_utc_ms = now_utc_ms();
        claim.lease_expires_utc_ms = claim.now_utc_ms + kLeaseMilliseconds;
        claim.maximum_plans = kMaximumClaimedPlans;
        std::vector<ports::PostBackupPlanRecord> plans;
        {
            auto unit = begin_unit_retry();
            if (!unit) {
                return;
            }
            auto claimed = unit.value()->post_backup_plans().claim_incomplete(claim, {});
            if (!claimed || !unit.value()->commit({})) {
                if (!claimed) {
                    unit.value()->rollback();
                }
                return;
            }
            plans = std::move(claimed).value();
        }
        for (auto& plan : plans) {
            const auto before_verify = plan.verify.state;
            const auto before_boot = plan.boot_check.state;
            const auto before_verify_attempts = plan.verify.attempts;
            const auto before_boot_attempts = plan.boot_check.attempts;
            drive_plan(plan);
            if (plan.verify.state != before_verify || plan.boot_check.state != before_boot ||
                plan.verify.attempts != before_verify_attempts ||
                plan.boot_check.attempts != before_boot_attempts ||
                ports::is_complete_post_backup_plan(plan)) {
                persist_plan(plan);
            }
        }
    }

    void run(const std::stop_token stopped) {
        write_log(logger, ServiceLogLevel::kInfo, "post_backup.coordinator_started",
                  "owner=" + claim_owner);
        while (!stopped.stop_requested()) {
            scan_once();
            std::unique_lock lock(mutex);
            changed.wait_for(lock, stopped, kScanInterval, [this] { return kicked || stopping; });
            kicked = false;
            if (stopping) {
                break;
            }
        }
        write_log(logger, ServiceLogLevel::kInfo, "post_backup.coordinator_stopped",
                  "owner=" + claim_owner);
    }
};

PostBackupCoordinator::PostBackupCoordinator(ports::IControlPlaneDatabase& control_plane,
                                             ports::IClock& clock, ports::IRandomSource& random,
                                             IServiceLog* const logger)
    : impl_(std::make_unique<Impl>(control_plane, clock, random, logger)) {}

PostBackupCoordinator::~PostBackupCoordinator() { shutdown(); }

void PostBackupCoordinator::start(WorkerJobService& worker_jobs,
                                  BootCheckSupervisor* const boot_check) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->scanner.joinable() || impl_->stopping) {
        return;
    }
    impl_->worker_jobs = &worker_jobs;
    impl_->boot_check = boot_check;
    impl_->claim_owner = random_owner_id(impl_->random);
    impl_->scanner =
        std::jthread([state = impl_.get()](const std::stop_token stopped) { state->run(stopped); });
}

void PostBackupCoordinator::kick() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        impl_->kicked = true;
    }
    impl_->changed.notify_all();
}

void PostBackupCoordinator::shutdown() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) {
            return;
        }
        impl_->stopping = true;
    }
    impl_->scanner.request_stop();
    impl_->changed.notify_all();
    if (impl_->scanner.joinable()) {
        impl_->scanner.join();
    }
}

} // namespace aegra::apps::service
