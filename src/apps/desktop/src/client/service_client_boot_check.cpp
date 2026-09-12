#include "client/service_client.h"

#include "client/service_protocol.h"
#include "locale/message_code_map.h"

#include <QDateTime>
#include <QUuid>

#include <cstdint>

namespace aegra::desktop {
namespace {

constexpr std::int64_t kOperationBootCheck = 5;
constexpr std::int64_t kJobStateQueued = 1;

[[nodiscard]] QString new_request_id() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

[[nodiscard]] QString new_idempotency_key() {
    return QStringLiteral("desktop-") + new_request_id();
}

} // namespace

bool ServiceClient::bootCheckAvailable() const {
    return connected() && capabilities_.contains(QStringLiteral("recovery_point.boot_check"));
}

// The Service command takes one recovery point; a multi-selection becomes a
// queue of commands submitted one at a time (like verify batches). Each accepted
// command records a Queued BootCheck job plus its durable plan (ADR-0032); the
// Service supervisor then runs them within the configured VM concurrency.
bool ServiceClient::bootCheckRecoveryPoints(const QStringList& recovery_point_ids) {
    if (!bootCheckAvailable() || repository_command_busy_ || recovery_point_ids.isEmpty() ||
        selected_repository_connection_id_.isEmpty()) {
        return false;
    }
    const auto known = recovery_points_.fileUuids();
    QStringList pending;
    for (const auto& id : recovery_point_ids) {
        if (id.isEmpty() || !known.contains(id) || pending.contains(id)) {
            return false;
        }
        pending.push_back(id);
    }
    repository_boot_check_pending_ = std::move(pending);
    repository_command_connection_id_ = selected_repository_connection_id_;
    repository_command_kind_ = kStartBootCheckRequestKind;
    repository_command_busy_ = true;
    repository_command_error_code_.clear();
    emit repositoryCommandChanged();
    return submit_next_repository_boot_check();
}

bool ServiceClient::submit_next_repository_boot_check() {
    repository_command_request_id_ = new_request_id();
    repository_command_idempotency_key_ = new_idempotency_key();
    const auto body = encode_start_boot_check_request(
        repository_command_request_id_, repository_command_idempotency_key_,
        repository_command_connection_id_, repository_boot_check_pending_.front());
    const auto started = coordinator_->begin_request(
        repository_command_request_id_, body,
        [this](const QByteArray& frame) { return handle_repository_command_frame(frame); });
    if (!started) {
        finish_repository_command_failure(QStringLiteral("service.send_failed"));
    }
    return started;
}

// Optimistic Queued row so the task list and the recovery point status column
// react before the next ListJobs poll; the Service row replaces it by job_id.
void ServiceClient::observe_accepted_boot_check_job(const QString& job_id,
                                                    const QString& recovery_point_id) {
    if (job_id.isEmpty()) {
        return;
    }
    JobRow optimistic;
    optimistic.job_id = job_id;
    optimistic.operation = kOperationBootCheck;
    optimistic.state = kJobStateQueued;
    optimistic.created_utc_ms = QDateTime::currentMSecsSinceEpoch();
    if (!recovery_point_id.isEmpty()) {
        optimistic.source_ids = {recovery_point_id};
        optimistic.progress_recovery_point_id = recovery_point_id;
    }
    optimistic.connection_id = repository_command_connection_id_;
    enrich_job_row(optimistic);
    jobs_.upsert_job(std::move(optimistic));
    emit jobsChanged();
    if (job_list_available_ && !jobs_loading_ &&
        (job_request_id_.isEmpty() || !coordinator_->has_pending_request(job_request_id_))) {
        start_job_query();
        return;
    }
    update_job_polling();
}

// Stable bootcheck.* reasons (provider unavailable, volume_set required) are
// shown verbatim; anything else falls back to the generic submission text.
void ServiceClient::finish_boot_check_submission_failure(const QString& message_code) {
    repository_boot_check_pending_.clear();
    const auto translation_id = translation_id_for_message_code(message_code);
    if (translation_id != QLatin1String("aegra.error.unknown")) {
        show_toast(localize_message_code(message_code), true);
        return;
    }
    //% "Boot check could not be submitted."
    show_toast(qtTrId("aegra.repository.bootcheck.submission_failed"), true);
}

} // namespace aegra::desktop
