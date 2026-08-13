#include "aegra/apps/service/schedule_service.h"

#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/file_browse_service.h"
#include "aegra/base/uuid.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

[[nodiscard]] unsigned utc_day_of_month(const std::uint64_t utc_ms) {
    using namespace std::chrono;
    const sys_seconds time_point{seconds{static_cast<std::int64_t>(utc_ms / 1000ULL)}};
    const year_month_day date{floor<days>(time_point)};
    return static_cast<unsigned>(date.day());
}

[[nodiscard]] bool month_day_is_selected(const std::uint32_t mask, const unsigned day) noexcept {
    return day >= 1 && day <= 31 && (mask & (1U << (day - 1U))) != 0;
}

[[nodiscard]] std::uint64_t
compute_next_run_for_minute(const contracts::ScheduleTrigger& trigger, const std::uint64_t now_ms,
                            const std::uint16_t local_minute) {
    constexpr std::uint64_t kDayMs = 24ULL * 60ULL * 60ULL * 1000ULL;
    constexpr std::uint64_t kMinuteMs = 60ULL * 1000ULL;
    const auto minute = static_cast<std::uint64_t>(local_minute);
    const auto day_start = (now_ms / kDayMs) * kDayMs;
    auto candidate = day_start + minute * kMinuteMs;
    if (candidate <= now_ms) {
        candidate += kDayMs;
    }
    if (trigger.kind == contracts::ScheduleTriggerKind::kWeekly && trigger.weekday_mask != 0) {
        for (int step = 0; step < 8; ++step) {
            const auto days_since_epoch = candidate / kDayMs;
            const auto weekday = static_cast<std::uint8_t>((days_since_epoch + 4U) % 7U);
            if ((trigger.weekday_mask & static_cast<std::uint8_t>(1U << weekday)) != 0) {
                break;
            }
            candidate += kDayMs;
        }
    }
    if (trigger.kind == contracts::ScheduleTriggerKind::kMonthly &&
        trigger.day_of_month_mask != 0) {
        for (int step = 0; step < 40; ++step) {
            if (month_day_is_selected(trigger.day_of_month_mask, utc_day_of_month(candidate))) {
                break;
            }
            candidate += kDayMs;
        }
    }
    return candidate;
}

[[nodiscard]] std::uint64_t compute_next_run_utc_ms(const contracts::ScheduleTrigger& trigger,
                                                    const std::uint64_t now_ms) {
    std::uint64_t best = 0;
    bool have_best = false;
    for (const auto minute : trigger.local_minutes_of_day) {
        const auto candidate = compute_next_run_for_minute(trigger, now_ms, minute);
        if (!have_best || candidate < best) {
            best = candidate;
            have_best = true;
        }
    }
    return have_best ? best : now_ms;
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

[[nodiscard]] std::string password_digest_token(const std::string_view password) {
    if (password.empty()) {
        return "none";
    }
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
    fingerprint += std::to_string(static_cast<int>(command.protection.content_kind));
    fingerprint += "|";
    for (const auto& source_id : command.protection.volume_source_ids) {
        fingerprint += std::to_string(source_id.size());
        fingerprint += ":";
        fingerprint += source_id;
        fingerprint += "|";
    }
    for (const auto& selection : command.protection.file_selections) {
        fingerprint += std::to_string(selection.node_token.size());
        fingerprint += ":";
        fingerprint += selection.node_token;
        fingerprint += ":";
        fingerprint += std::to_string(static_cast<int>(selection.recursion));
        fingerprint += "|";
    }
    fingerprint += command.repository_connection_id;
    fingerprint += "|";
    fingerprint += std::to_string(static_cast<int>(command.backup_type));
    fingerprint += "|";
    fingerprint += std::to_string(static_cast<int>(command.trigger.kind));
    fingerprint += "|";
    for (std::size_t index = 0; index < command.trigger.local_minutes_of_day.size(); ++index) {
        if (index != 0) {
            fingerprint.push_back(',');
        }
        fingerprint += std::to_string(command.trigger.local_minutes_of_day[index]);
    }
    fingerprint += "|";
    fingerprint += std::to_string(command.trigger.weekday_mask);
    fingerprint += "|";
    fingerprint += std::to_string(command.trigger.day_of_month_mask);
    fingerprint += "|";
    fingerprint += command.trigger.timezone_id;
    fingerprint += "|";
    fingerprint += command.exclude_page_and_hibernation_files ? "1" : "0";
    fingerprint += "|";
    fingerprint += command.deduplication_enabled ? "1" : "0";
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

[[nodiscard]] std::string path_key(const contracts::FileSourceRef& ref) {
    std::string key = ref.volume_identity;
    key.push_back('|');
    for (const auto& component : ref.relative_components) {
        key.push_back('/');
        for (const auto byte : component.bytes) {
            key.push_back(static_cast<char>(std::to_integer<unsigned>(byte)));
        }
    }
    key.push_back('|');
    key += std::to_string(static_cast<int>(ref.recursion));
    return key;
}

[[nodiscard]] base::Result<std::vector<contracts::FileSourceRef>>
normalize_file_selections(std::vector<contracts::FileSourceRef> selections) {
    std::ranges::sort(selections, [](const contracts::FileSourceRef& left,
                                     const contracts::FileSourceRef& right) {
        if (left.volume_identity != right.volume_identity) {
            return left.volume_identity < right.volume_identity;
        }
        if (left.relative_components.size() != right.relative_components.size()) {
            return left.relative_components.size() < right.relative_components.size();
        }
        return path_key(left) < path_key(right);
    });
    std::vector<contracts::FileSourceRef> unique;
    unique.reserve(selections.size());
    std::map<std::string, std::size_t> seen;
    for (auto& selection : selections) {
        const auto key = path_key(selection);
        const auto existing = seen.find(key);
        if (existing != seen.end()) {
            return base::Result<std::vector<contracts::FileSourceRef>>::failure(
                {base::ErrorCode::kConflict, "file protection selection is duplicate"});
        }
        seen.emplace(key, unique.size());
        unique.push_back(std::move(selection));
    }
    // Drop children covered by a recursive parent on the same volume.
    std::vector<contracts::FileSourceRef> pruned;
    for (std::size_t index = 0; index < unique.size(); ++index) {
        bool covered = false;
        for (std::size_t parent = 0; parent < unique.size(); ++parent) {
            if (parent == index) {
                continue;
            }
            const auto& candidate = unique[parent];
            const auto& child = unique[index];
            if (candidate.volume_identity != child.volume_identity ||
                candidate.recursion != contracts::FileRecursion::kRecursive ||
                candidate.entry_kind != contracts::FileEntryKind::kDirectory ||
                candidate.relative_components.size() >= child.relative_components.size()) {
                continue;
            }
            bool prefix = true;
            for (std::size_t part = 0; part < candidate.relative_components.size(); ++part) {
                if (candidate.relative_components[part].bytes !=
                    child.relative_components[part].bytes) {
                    prefix = false;
                    break;
                }
            }
            if (prefix) {
                covered = true;
                break;
            }
        }
        if (!covered) {
            pruned.push_back(std::move(unique[index]));
        }
    }
    if (pruned.empty()) {
        return base::Result<std::vector<contracts::FileSourceRef>>::failure(
            {base::ErrorCode::kInvalidArgument, "file protection selections are empty after prune"});
    }
    return base::Result<std::vector<contracts::FileSourceRef>>::success(std::move(pruned));
}

[[nodiscard]] base::Result<void>
enforce_schedule_update_invariants(const contracts::UpsertScheduleCommand& command,
                                   const ports::ScheduleRecord& existing) {
    if (command.protection.content_kind != existing.content_kind) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument, "schedule content_kind cannot be changed after create"});
    }
    if (existing.content_kind == contracts::ContentKind::kVolumeSet) {
        if (command.protection.volume_source_ids != existing.source_ids) {
            return base::Result<void>::failure(
                {base::ErrorCode::kInvalidArgument, "schedule sources cannot be changed after create"});
        }
    } else if (!command.protection.file_selections.empty()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "schedule.source_frozen"});
    }
    if (command.exclude_page_and_hibernation_files != existing.exclude_page_and_hibernation_files) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "schedule backup options cannot be changed after create"});
    }
    if (command.deduplication_enabled != existing.deduplication_enabled) {
        return base::Result<void>::failure(
            {base::ErrorCode::kInvalidArgument,
             "schedule deduplication cannot be changed after create"});
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
                                 ports::IRandomSource& random,
                                 application::FileBrowseService* const file_browse) noexcept
    : control_plane_(control_plane), clock_(clock), random_(random), file_browse_(file_browse) {}

base::Result<contracts::SchedulePage>
ScheduleService::list_schedules(const contracts::ScheduleListRequest& request,
                                const base::CancellationToken cancellation) {
    return control_plane_.list_schedules(request, cancellation);
}

base::Result<contracts::CommandAcknowledgement>
ScheduleService::upsert_schedule(const contracts::UpsertScheduleCommand& command,
                                 const std::string_view idempotency_key,
                                 const ports::FileBrowseCaller& caller,
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

    std::vector<contracts::FileSourceRef> resolved_files;
    if (command.protection.content_kind == contracts::ContentKind::kFileSet && !command.schedule_id) {
        if (file_browse_ == nullptr) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "file_set schedules are not enabled"});
        }
        resolved_files.reserve(command.protection.file_selections.size());
        for (const auto& selection : command.protection.file_selections) {
            auto resolved = file_browse_->resolve_selection(
                caller, selection.node_token, selection.recursion, selection.display_label,
                cancellation);
            if (!resolved) {
                return base::Result<contracts::CommandAcknowledgement>::failure(resolved.error());
            }
            resolved.value().unreadable_policy = command.protection.file_options.unreadable_policy;
            resolved_files.push_back(std::move(resolved).value());
        }
        auto normalized = normalize_file_selections(std::move(resolved_files));
        if (!normalized) {
            return base::Result<contracts::CommandAcknowledgement>::failure(normalized.error());
        }
        resolved_files = std::move(normalized).value();
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
    std::string owner_sid = caller.caller_sid;
    std::vector<std::string> source_ids;
    std::vector<contracts::FileSourceRef> file_selections;
    contracts::ContentKind content_kind = command.protection.content_kind;
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
        backup_set_uuid = existing.value()->backup_set_uuid;
        archive_password_protected = existing.value()->archive_password_protected;
        owner_sid = existing.value()->owner_sid;
        content_kind = existing.value()->content_kind;
        source_ids = existing.value()->source_ids;
        file_selections = existing.value()->file_selections;
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
        if (content_kind == contracts::ContentKind::kVolumeSet) {
            source_ids = command.protection.volume_source_ids;
        } else {
            file_selections = std::move(resolved_files);
        }
        if (command.encryption_enabled) {
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
    record.content_kind = content_kind;
    record.source_ids = std::move(source_ids);
    record.file_selections = std::move(file_selections);
    record.owner_sid = std::move(owner_sid);
    record.repository_connection_id = command.repository_connection_id;
    // Scheduled runs always request Incremental; first backup / missing parent demote to Full.
    record.backup_type = contracts::BackupType::kIncremental;
    record.trigger = command.trigger;
    record.exclude_page_and_hibernation_files = command.exclude_page_and_hibernation_files;
    if (content_kind == contracts::ContentKind::kFileSet) {
        record.deduplication_enabled = false;
    } else {
        record.deduplication_enabled = command.deduplication_enabled;
    }
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
