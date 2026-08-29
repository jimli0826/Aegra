#include "aegra/apps/service/post_backup_verifier.h"

#include "aegra/apps/service/service_host.h"
#include "aegra/apps/service/worker_job_service.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace aegra::apps::service {
namespace {

constexpr std::size_t kMaximumPendingVerifications = 256;
constexpr auto kCapacityRetryDelay = std::chrono::milliseconds(250);

[[nodiscard]] bool is_capacity_conflict(const base::Error& error) noexcept {
    return error.code == base::ErrorCode::kConflict &&
           error.message.find("worker capacity") != std::string::npos;
}

void write_log(IServiceLog* const logger, const ServiceLogLevel level, const std::string_view code,
               const std::string_view detail) noexcept {
    if (logger == nullptr) {
        return;
    }
    logger->write(level, code, detail);
}

} // namespace

struct PostBackupVerifier::Impl final {
    explicit Impl(IServiceLog* const service_logger) noexcept : logger(service_logger) {}

    IServiceLog* logger{nullptr};
    WorkerJobService* worker_jobs{nullptr};
    std::mutex mutex;
    std::condition_variable changed;
    std::deque<WorkerJobRequest> pending;
    bool accepting{true};
    bool started{false};
    std::jthread dispatcher;

    [[nodiscard]] bool wait_for_capacity_retry(const std::stop_token stop) {
        std::unique_lock lock(mutex);
        return !changed.wait_for(lock, kCapacityRetryDelay,
                                 [&] { return stop.stop_requested() || !accepting; });
    }

    void submit_one(const std::stop_token stop, const WorkerJobRequest& request) {
        while (!stop.stop_requested()) {
            auto submitted = worker_jobs->start_post_backup_verify(request, {});
            if (submitted) {
                std::string detail = "Post-backup Verify submitted for backup job ";
                detail += request.worker_request.job_id;
                detail += "; verify_job=";
                detail += submitted.value().resource_id.value_or("");
                write_log(logger, ServiceLogLevel::kInfo, "post_backup.verify_submitted", detail);
                return;
            }
            if (is_capacity_conflict(submitted.error()) && wait_for_capacity_retry(stop)) {
                continue;
            }
            std::string detail = "Post-backup Verify submission failed for backup job ";
            detail += request.worker_request.job_id;
            detail += "; error=";
            detail += base::error_code_name(submitted.error().code);
            // Error messages are stable internal strings and never carry credential material.
            if (!submitted.error().message.empty()) {
                detail += "; detail=";
                detail += submitted.error().message;
            }
            write_log(logger, ServiceLogLevel::kError, "post_backup.verify_submit_failed", detail);
            return;
        }
    }

    void run(const std::stop_token stop) {
        while (!stop.stop_requested()) {
            WorkerJobRequest request;
            {
                std::unique_lock lock(mutex);
                changed.wait(
                    lock, [&] { return stop.stop_requested() || !accepting || !pending.empty(); });
                if (stop.stop_requested() || (!accepting && pending.empty())) {
                    return;
                }
                request = std::move(pending.front());
                pending.pop_front();
            }
            submit_one(stop, request);
        }
    }
};

PostBackupVerifier::PostBackupVerifier(IServiceLog* const logger)
    : impl_(std::make_unique<Impl>(logger)) {}

PostBackupVerifier::~PostBackupVerifier() { shutdown(); }

void PostBackupVerifier::start(WorkerJobService& worker_jobs) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->started || !impl_->accepting) {
        return;
    }
    impl_->worker_jobs = &worker_jobs;
    impl_->started = true;
    impl_->dispatcher =
        std::jthread([state = impl_.get()](const std::stop_token stop) { state->run(stop); });
    impl_->changed.notify_all();
}

bool PostBackupVerifier::enqueue(const WorkerJobRequest& completed_backup) {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->accepting || !completed_backup.verify_after_backup ||
        impl_->pending.size() >= kMaximumPendingVerifications) {
        return false;
    }
    impl_->pending.push_back(completed_backup);
    impl_->changed.notify_one();
    return true;
}

void PostBackupVerifier::shutdown() noexcept {
    {
        std::lock_guard lock(impl_->mutex);
        if (!impl_->accepting) {
            return;
        }
        impl_->accepting = false;
        impl_->pending.clear();
    }
    impl_->dispatcher.request_stop();
    impl_->changed.notify_all();
    if (impl_->dispatcher.joinable()) {
        impl_->dispatcher.join();
    }
}

} // namespace aegra::apps::service
