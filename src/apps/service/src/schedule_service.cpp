#include "aegra/apps/service/schedule_service.h"

#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/base/uuid.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::apps::service {
namespace {

[[nodiscard]] std::uint64_t utc_ms(const ports::IClock& clock) {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0 : static_cast<std::uint64_t>(now);
}

[[nodiscard]] base::Result<std::string> make_id(const std::string_view prefix,
                                                ports::IRandomSource& random,
                                                const base::CancellationToken& cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string id(prefix);
    id.reserve(prefix.size() + bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        id.push_back(kHex[value >> 4U]);
        id.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(id));
}

[[nodiscard]] base::Result<std::string>
make_canonical_uuid(ports::IRandomSource& random, const base::CancellationToken& cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    // RFC 4122 version 4 / variant 1.
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    return base::Result<std::string>::success(base::format_uuid(bytes));
}

[[nodiscard]] base::Result<void> require_idempotency_key(const std::string_view key) {
    if (key.empty() || key.size() > 128) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is invalid"});
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::uint64_t compute_next_run_utc_ms(const contracts::ScheduleTrigger& trigger,
                                                    const std::uint64_t now_ms) {
    // Personal edition: next daily/weekly fire as next wall-clock occurrence of local_minute_of_day
    // measured in UTC minutes (timezone conversion deferred).
    constexpr std::uint64_t kDayMs = 24ULL * 60ULL * 60ULL * 1000ULL;
    constexpr std::uint64_t kMinuteMs = 60ULL * 1000ULL;
    const auto minute = static_cast<std::uint64_t>(trigger.local_minute_of_day);
    const auto day_start = (now_ms / kDayMs) * kDayMs;
    auto candidate = day_start + minute * kMinuteMs;
    if (candidate <= now_ms) {
        candidate += kDayMs;
    }
    if (trigger.kind == contracts::ScheduleTriggerKind::kWeekly && trigger.weekday_mask != 0) {
        // Advance day-by-day until weekday_mask bit matches (bit0=Sunday … bit6=Saturday).
        for (int step = 0; step < 8; ++step) {
            const auto days_since_epoch = candidate / kDayMs;
            // 1970-01-01 was Thursday (4). Convert to Sunday=0.
            const auto weekday = static_cast<std::uint8_t>((days_since_epoch + 4U) % 7U);
            if ((trigger.weekday_mask & static_cast<std::uint8_t>(1U << weekday)) != 0) {
                break;
            }
            candidate += kDayMs;
        }
    }
    return candidate;
}

[[nodiscard]] contracts::CommandAcknowledgement accepted(std::string command_id,
                                                         std::string resource_id) {
    contracts::CommandAcknowledgement acknowledgement;
    acknowledgement.command_id = std::move(command_id);
    acknowledgement.disposition = contracts::CommandDisposition::kAccepted;
    acknowledgement.resource_id = std::move(resource_id);
    return acknowledgement;
}

[[nodiscard]] contracts::CommandAcknowledgement replayed(std::string command_id,
                                                         std::optional<std::string> resource_id) {
    contracts::CommandAcknowledgement acknowledgement;
    acknowledgement.command_id = std::move(command_id);
    acknowledgement.disposition = contracts::CommandDisposition::kReplayed;
    acknowledgement.resource_id = std::move(resource_id);
    return acknowledgement;
}

[[nodiscard]] ports::CommandRecord make_command_record(const std::string_view key,
                                                       std::string fingerprint,
                                                       const contracts::CommandAcknowledgement& ack,
                                                       const std::uint64_t now_ms) {
    return ports::CommandRecord{
        .idempotency_key = std::string(key),
        .request_fingerprint = std::move(fingerprint),
        .command_id = ack.command_id,
        .resource_id = ack.resource_id,
        .created_utc_ms = now_ms,
    };
}

/// Irreversible password identity for fingerprint only — never stores plaintext.
[[nodiscard]] std::string password_digest_token(const std::string_view password) {
    if (password.empty()) {
        return "none";
    }
    // FNV-1a 64-bit; distinguishes same-key password differences without retaining the secret.
    std::uint64_t hash = 14695981039346656037ULL;
    for (const char ch : password) {
        hash ^= static_cast<std::uint64_t>(static_cast<unsigned char>(ch));
        hash *= 1099511628211ULL;
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string token(16, '0');
    for (int i = 15; i >= 0; --i) {
        token[static_cast<std::size_t>(i)] = kHex[hash & 0xFU];
        hash >>= 4U;
    }
    return token;
}

[[nodiscard]] std::string upsert_fingerprint(const contracts::UpsertScheduleCommand& command) {
    std::string fingerprint = "upsert-schedule|";
    fingerprint += command.schedule_id.value_or("");
    fingerprint += "|";
    fingerprint += command.display_name;
    fingerprint += "|";
    fingerprint += command.enabled ? "1" : "0";
    fingerprint += "|";
    for (const auto& source_id : command.source_ids) {
        fingerprint += std::to_string(source_id.size());
        fingerprint += ":";
        fingerprint += source_id;
        fingerprint += "|";
    }
    fingerprint += command.repository_connection_id;
    fingerprint += "|";
    fingerprint += std::to_string(static_cast<int>(command.backup_type));
    fingerprint += "|";
    fingerprint += std::to_string(static_cast<int>(command.trigger.kind));
    fingerprint += "|";
    fingerprint += std::to_string(command.trigger.local_minute_of_day);
    fingerprint += "|";
    fingerprint += std::to_string(command.trigger.weekday_mask);
    fingerprint += "|";
    fingerprint += command.trigger.timezone_id;
    fingerprint += "|";
    fingerprint += command.exclude_page_and_hibernation_files ? "1" : "0";
    fingerprint += "|";
    fingerprint += command.encryption_enabled ? "1" : "0";
    fingerprint += "|pwd:";
    fingerprint += password_digest_token(command.archive_password);
    return fingerprint;
}

[[nodiscard]] base::Result<contracts::CommandAcknowledgement>
replay_if_same_request(const ports::CommandRecord& existing, const std::string& fingerprint) {
    if (existing.request_fingerprint != fingerprint) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "idempotency key request mismatch"});
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        replayed(existing.command_id, existing.resource_id));
}

/// Create freezes sources, encryption, password, and other backup options (except future
/// shutdown-on-complete). Update may change repository, schedule settings, enabled, display name.
[[nodiscard]] base::Result<void>
enforce_schedule_update_invariants(const contracts::UpsertScheduleCommand& command,
                                   const ports::ScheduleRecord& existing) {
    if (command.source_ids != existing.source_ids) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "schedule sources cannot be changed after create"});
    }
    if (command.backup_type != existing.backup_type) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "schedule backup type cannot be changed after create"});
    }
    if (command.exclude_page_and_hibernation_files != existing.exclude_page_and_hibernation_files) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "schedule backup options cannot be changed after create"});
    }
    if (command.encryption_enabled != existing.encryption_enabled) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "schedule encryption cannot be changed after create"});
    }
    if (!command.archive_password.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "schedule password cannot be changed after create"});
    }
    return base::Result<void>::success();
}

} // namespace

ScheduleService::ScheduleService(ports::IControlPlaneDatabase& control_plane, ports::IClock& clock,
                                 ports::IRandomSource& random) noexcept
    : control_plane_(control_plane), clock_(clock), random_(random) {}

base::Result<contracts::SchedulePage>
ScheduleService::list_schedules(const contracts::ScheduleListRequest& request,
                                const base::CancellationToken cancellation) {
    return control_plane_.list_schedules(request, cancellation);
}

base::Result<contracts::CommandAcknowledgement>
ScheduleService::upsert_schedule(const contracts::UpsertScheduleCommand& command,
                                 const std::string_view idempotency_key,
                                 const base::CancellationToken cancellation) {
    if (auto key = require_idempotency_key(idempotency_key); !key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    auto valid = contracts::validate_upsert_schedule_command(command);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }

    const auto fingerprint = upsert_fingerprint(command);
    auto existing_command = control_plane_.get_command(idempotency_key, cancellation);
    if (!existing_command) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing_command.error());
    }
    if (existing_command.value()) {
        return replay_if_same_request(*existing_command.value(), fingerprint);
    }

    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    const auto now = utc_ms(clock_);
    auto command_id = make_id("cmd-", random_, cancellation);
    if (!command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }

    std::string schedule_id;
    std::string backup_set_uuid;
    std::string archive_password_protected;
    std::optional<std::string> last_recovery_point_id;
    std::uint64_t created_utc_ms = now;
    if (command.schedule_id) {
        schedule_id = *command.schedule_id;
        auto existing = unit.value()->schedules().get(schedule_id, cancellation);
        if (!existing) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
        }
        if (!existing.value()) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kNotFound, "schedule id not found"});
        }
        if (auto frozen = enforce_schedule_update_invariants(command, *existing.value()); !frozen) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(frozen.error());
        }
        created_utc_ms = existing.value()->created_utc_ms;
        // Chain identity and protected password are fixed for the lifetime of the schedule.
        backup_set_uuid = existing.value()->backup_set_uuid;
        archive_password_protected = existing.value()->archive_password_protected;
        // Tip is runtime state: preserve across edits; clear when repository connection changes.
        if (command.repository_connection_id == existing.value()->repository_connection_id) {
            last_recovery_point_id = existing.value()->last_recovery_point_id;
        }
    } else {
        auto generated = make_id("sch-", random_, cancellation);
        if (!generated) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(generated.error());
        }
        schedule_id = std::move(generated).value();
        auto set_uuid = make_canonical_uuid(random_, cancellation);
        if (!set_uuid) {
            unit.value()->rollback();
            return base::Result<contracts::CommandAcknowledgement>::failure(set_uuid.error());
        }
        backup_set_uuid = std::move(set_uuid).value();
        if (command.encryption_enabled) {
            // pOptionalEntropy = schedule_id so ciphertext is bound to this schedule.
            auto protected_secret = adapters::windows_system::protect_local_machine_secret(
                command.archive_password, schedule_id);
            if (!protected_secret) {
                unit.value()->rollback();
                return base::Result<contracts::CommandAcknowledgement>::failure(
                    protected_secret.error());
            }
            archive_password_protected = std::move(protected_secret).value().value;
        }
    }

    ports::ScheduleRecord record;
    record.schedule_id = schedule_id;
    record.display_name = command.display_name;
    record.enabled = command.enabled;
    record.source_ids = command.source_ids;
    record.repository_connection_id = command.repository_connection_id;
    record.backup_type = command.backup_type;
    record.trigger = command.trigger;
    record.exclude_page_and_hibernation_files = command.exclude_page_and_hibernation_files;
    record.encryption_enabled = command.encryption_enabled;
    record.archive_password_protected = std::move(archive_password_protected);
    record.backup_set_uuid = std::move(backup_set_uuid);
    record.last_recovery_point_id = std::move(last_recovery_point_id);
    if (record.trigger.timezone_id.empty()) {
        record.trigger.timezone_id = "UTC";
    }
    if (record.enabled) {
        record.next_run_utc_ms = compute_next_run_utc_ms(record.trigger, now);
    } else {
        record.next_run_utc_ms = std::nullopt;
    }
    record.created_utc_ms = created_utc_ms;
    record.updated_utc_ms = now;

    auto upserted = unit.value()->schedules().upsert(record, cancellation);
    if (!upserted) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(upserted.error());
    }

    auto acknowledgement = accepted(std::move(command_id).value(), schedule_id);
    auto stored = unit.value()->commands().insert(
        make_command_record(idempotency_key, fingerprint, acknowledgement, now), cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(
                           std::move(acknowledgement))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

base::Result<contracts::CommandAcknowledgement>
ScheduleService::delete_schedule(const contracts::ResourceRef& reference,
                                 const std::string_view idempotency_key,
                                 const base::CancellationToken cancellation) {
    if (auto key = require_idempotency_key(idempotency_key); !key) {
        return base::Result<contracts::CommandAcknowledgement>::failure(key.error());
    }
    auto valid = contracts::validate_resource_ref(reference);
    if (!valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }

    const auto fingerprint = std::string("delete-schedule|") + reference.resource_id;
    auto existing_command = control_plane_.get_command(idempotency_key, cancellation);
    if (!existing_command) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing_command.error());
    }
    if (existing_command.value()) {
        return replay_if_same_request(*existing_command.value(), fingerprint);
    }

    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::CommandAcknowledgement>::failure(unit.error());
    }
    const auto now = utc_ms(clock_);
    auto command_id = make_id("cmd-", random_, cancellation);
    if (!command_id) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(command_id.error());
    }
    auto removed = unit.value()->schedules().remove(reference.resource_id, cancellation);
    if (!removed) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(removed.error());
    }
    auto acknowledgement = accepted(std::move(command_id).value(), reference.resource_id);
    auto stored = unit.value()->commands().insert(
        make_command_record(idempotency_key, fingerprint, acknowledgement, now), cancellation);
    if (!stored) {
        unit.value()->rollback();
        return base::Result<contracts::CommandAcknowledgement>::failure(stored.error());
    }
    auto committed = unit.value()->commit(cancellation);
    return committed ? base::Result<contracts::CommandAcknowledgement>::success(
                           std::move(acknowledgement))
                     : base::Result<contracts::CommandAcknowledgement>::failure(committed.error());
}

} // namespace aegra::apps::service
