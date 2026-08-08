#include "sqlite_internal.h"

#include "aegra/base/uuid.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/service_control.h"

#include <algorithm>
#include <cctype>
#include <limits>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr std::size_t kMaximumIdentifierBytes = 128;
constexpr std::size_t kMaximumDisplayNameBytes = 256;
constexpr std::size_t kMaximumLocatorBytes = 2'048;
constexpr std::size_t kMaximumMessageCodeBytes = 128;
constexpr std::size_t kMaximumMessageArguments = 16;
constexpr std::size_t kMaximumMessageArgumentBytes = 256;
constexpr std::size_t kMaximumCapabilities = 64;
constexpr std::size_t kMaximumCapabilityBytes = 64;
constexpr std::size_t kMaximumTimezoneBytes = 128;
constexpr std::size_t kMaximumTokenBytes = 1'024;
constexpr std::size_t kMaximumCommandFingerprintBytes = 4'096;

[[nodiscard]] bool valid_stable_character(const unsigned char value) noexcept {
    return (value >= 'a' && value <= 'z') || (value >= '0' && value <= '9') || value == '.' ||
           value == '_' || value == '-' || value == ':';
}

[[nodiscard]] bool valid_stable_value(const std::string_view value,
                                      const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::ranges::all_of(value, valid_stable_character);
}

[[nodiscard]] bool valid_text(const std::string_view value,
                              const std::size_t maximum_bytes) noexcept {
    return !value.empty() && value.size() <= maximum_bytes &&
           std::ranges::all_of(value, [](const unsigned char character) {
               return character >= 0x20U && character != 0x7FU;
           });
}

[[nodiscard]] bool valid_source_ids(const std::vector<std::string>& source_ids,
                                    const bool allow_empty) {
    if ((!allow_empty && source_ids.empty()) ||
        source_ids.size() > contracts::kMaximumBackupSources) {
        return false;
    }
    std::set<std::string_view> seen;
    return std::ranges::all_of(source_ids, [&seen](const std::string& source_id) {
        return valid_stable_value(source_id, kMaximumIdentifierBytes) &&
               seen.insert(source_id).second;
    });
}

[[nodiscard]] bool valid_wire_integer(const std::uint64_t value) noexcept {
    return value <= static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
}

[[nodiscard]] bool valid_optional_wire_integer(const std::optional<std::uint64_t>& value) noexcept {
    return !value || valid_wire_integer(*value);
}

[[nodiscard]] bool known_job_state(const contracts::ServiceJobState state) noexcept {
    return state >= contracts::ServiceJobState::kQueued &&
           state <= contracts::ServiceJobState::kInterrupted;
}

[[nodiscard]] bool known_job_operation(const contracts::JobOperation operation) noexcept {
    return operation == contracts::JobOperation::kBackup ||
           operation == contracts::JobOperation::kRestore ||
           operation == contracts::JobOperation::kVerify ||
           operation == contracts::JobOperation::kExport;
}

[[nodiscard]] bool known_backup_type(const contracts::BackupType type) noexcept {
    return type == contracts::BackupType::kFull || type == contracts::BackupType::kIncremental ||
           type == contracts::BackupType::kDifferential;
}

[[nodiscard]] bool
known_repository_state(const contracts::RepositoryConnectionState state) noexcept {
    return state == contracts::RepositoryConnectionState::kAvailable ||
           state == contracts::RepositoryConnectionState::kUnavailable;
}

[[nodiscard]] bool known_audit_severity(const contracts::AuditSeverity severity) noexcept {
    return severity >= contracts::AuditSeverity::kInformation &&
           severity <= contracts::AuditSeverity::kCritical;
}

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure(make_error(base::ErrorCode::kInvalidArgument, message));
}

[[nodiscard]] char hex_digit(const unsigned value) noexcept {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

[[nodiscard]] int hex_value(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] std::string percent_encode(const std::string_view value) {
    std::string encoded;
    encoded.reserve(value.size());
    for (const unsigned char character : value) {
        if (std::isalnum(character) != 0 || character == '-' || character == '_' ||
            character == '.' || character == ':') {
            encoded.push_back(static_cast<char>(character));
            continue;
        }
        encoded.push_back('%');
        encoded.push_back(hex_digit((character >> 4U) & 0x0FU));
        encoded.push_back(hex_digit(character & 0x0FU));
    }
    return encoded;
}

[[nodiscard]] base::Result<std::string> percent_decode(const std::string_view value) {
    std::string decoded;
    decoded.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        const char character = value[index];
        if (character != '%') {
            decoded.push_back(character);
            continue;
        }
        if (index + 2 >= value.size()) {
            return base::Result<std::string>::failure(
                make_error(base::ErrorCode::kCorruptData, "encoded list is corrupt"));
        }
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            return base::Result<std::string>::failure(
                make_error(base::ErrorCode::kCorruptData, "encoded list is corrupt"));
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return base::Result<std::string>::success(std::move(decoded));
}

} // namespace

base::Error make_error(const base::ErrorCode code, const char* message) { return {code, message}; }

base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCancelled, "control plane operation cancelled"));
    }
    return base::Result<void>::success();
}

base::Result<void> check_unit_of_work_active(const bool* const active) {
    if (active != nullptr && !*active) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kConflict, "unit of work already finished"));
    }
    return base::Result<void>::success();
}

base::Result<void> map_sqlite_result(const int rc, sqlite3* const db, const char* const context) {
    if (rc == SQLITE_OK || rc == SQLITE_DONE || rc == SQLITE_ROW) {
        return base::Result<void>::success();
    }
    if (rc == SQLITE_BUSY || rc == SQLITE_LOCKED) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kConflict, "control plane database is busy"));
    }
    if (rc == SQLITE_CORRUPT || rc == SQLITE_NOTADB) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kCorruptData, "control plane database is corrupt"));
    }
    if (rc == SQLITE_CONSTRAINT) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kConflict, "control plane constraint violated"));
    }
    if (rc == SQLITE_FULL || rc == SQLITE_IOERR || rc == SQLITE_CANTOPEN) {
        return base::Result<void>::failure(make_error(
            base::ErrorCode::kIoFailure, context != nullptr ? context : "sqlite io failure"));
    }
    (void)db;
    return base::Result<void>::failure(
        make_error(base::ErrorCode::kInternal, context != nullptr ? context : "sqlite failure"));
}

SqliteStatement::SqliteStatement(sqlite3_stmt* const stmt) noexcept : stmt_(stmt) {}

SqliteStatement::SqliteStatement(SqliteStatement&& other) noexcept : stmt_(other.stmt_) {
    other.stmt_ = nullptr;
}

SqliteStatement& SqliteStatement::operator=(SqliteStatement&& other) noexcept {
    if (this != &other) {
        finalize();
        stmt_ = other.stmt_;
        other.stmt_ = nullptr;
    }
    return *this;
}

SqliteStatement::~SqliteStatement() { finalize(); }

void SqliteStatement::finalize() noexcept {
    if (stmt_ != nullptr) {
        sqlite3_finalize(stmt_);
        stmt_ = nullptr;
    }
}

base::Result<SqliteStatement> SqliteStatement::prepare(sqlite3* const db,
                                                       const std::string_view sql) {
    sqlite3_stmt* stmt = nullptr;
    const int rc = sqlite3_prepare_v2(db, sql.data(), static_cast<int>(sql.size()), &stmt, nullptr);
    auto mapped = map_sqlite_result(rc, db, "prepare statement failed");
    if (!mapped) {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
        return base::Result<SqliteStatement>::failure(mapped.error());
    }
    return base::Result<SqliteStatement>::success(SqliteStatement{stmt});
}

base::Result<void> SqliteStatement::bind_text(const int index, const std::string_view value) {
    const int rc = sqlite3_bind_text(stmt_, index, value.data(), static_cast<int>(value.size()),
                                     SQLITE_TRANSIENT);
    return map_sqlite_result(rc, sqlite3_db_handle(stmt_), "bind text failed");
}

base::Result<void> SqliteStatement::bind_text_nullable(const int index,
                                                       const std::optional<std::string>& value) {
    if (!value) {
        return bind_null(index);
    }
    return bind_text(index, *value);
}

base::Result<void> SqliteStatement::bind_int64(const int index, const std::int64_t value) {
    return map_sqlite_result(sqlite3_bind_int64(stmt_, index, value), sqlite3_db_handle(stmt_),
                             "bind int64 failed");
}

base::Result<void> SqliteStatement::bind_int64_nullable(const int index,
                                                        const std::optional<std::uint64_t>& value) {
    if (!value) {
        return bind_null(index);
    }
    if (!valid_wire_integer(*value)) {
        return invalid("timestamp exceeds wire integer range");
    }
    return bind_int64(index, static_cast<std::int64_t>(*value));
}

base::Result<void> SqliteStatement::bind_null(const int index) {
    return map_sqlite_result(sqlite3_bind_null(stmt_, index), sqlite3_db_handle(stmt_),
                             "bind null failed");
}

base::Result<int> SqliteStatement::step() {
    const int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW || rc == SQLITE_DONE) {
        return base::Result<int>::success(rc);
    }
    auto mapped = map_sqlite_result(rc, sqlite3_db_handle(stmt_), "step failed");
    return base::Result<int>::failure(mapped.error());
}

base::Result<void> SqliteStatement::reset() {
    const int rc = sqlite3_reset(stmt_);
    if (rc != SQLITE_OK) {
        return map_sqlite_result(rc, sqlite3_db_handle(stmt_), "reset failed");
    }
    return map_sqlite_result(sqlite3_clear_bindings(stmt_), sqlite3_db_handle(stmt_),
                             "clear bindings failed");
}

std::string encode_string_list(const std::vector<std::string>& values) {
    std::string encoded;
    for (std::size_t index = 0; index < values.size(); ++index) {
        if (index != 0) {
            encoded.push_back('|');
        }
        encoded.append(percent_encode(values[index]));
    }
    return encoded;
}

base::Result<std::vector<std::string>> decode_string_list(const std::string_view encoded) {
    std::vector<std::string> values;
    if (encoded.empty()) {
        return base::Result<std::vector<std::string>>::success(std::move(values));
    }
    std::size_t begin = 0;
    while (begin <= encoded.size()) {
        const auto end = encoded.find('|', begin);
        const auto part = encoded.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        auto decoded = percent_decode(part);
        if (!decoded) {
            return base::Result<std::vector<std::string>>::failure(decoded.error());
        }
        values.push_back(std::move(decoded.value()));
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return base::Result<std::vector<std::string>>::success(std::move(values));
}

std::string encode_message_arguments(const contracts::MessageArguments& arguments) {
    std::string encoded;
    for (std::size_t index = 0; index < arguments.size(); ++index) {
        if (index != 0) {
            encoded.push_back(';');
        }
        encoded.append(percent_encode(arguments[index].name));
        encoded.push_back('=');
        encoded.append(percent_encode(arguments[index].value));
    }
    return encoded;
}

base::Result<contracts::MessageArguments> decode_message_arguments(const std::string_view encoded) {
    contracts::MessageArguments arguments;
    if (encoded.empty()) {
        return base::Result<contracts::MessageArguments>::success(std::move(arguments));
    }
    std::size_t begin = 0;
    while (begin <= encoded.size()) {
        const auto end = encoded.find(';', begin);
        const auto part = encoded.substr(
            begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
        const auto separator = part.find('=');
        if (separator == std::string_view::npos) {
            return base::Result<contracts::MessageArguments>::failure(
                make_error(base::ErrorCode::kCorruptData, "message arguments are corrupt"));
        }
        auto name = percent_decode(part.substr(0, separator));
        auto value = percent_decode(part.substr(separator + 1));
        if (!name || !value) {
            return base::Result<contracts::MessageArguments>::failure(
                make_error(base::ErrorCode::kCorruptData, "message arguments are corrupt"));
        }
        arguments.push_back({std::move(name.value()), std::move(value.value())});
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return base::Result<contracts::MessageArguments>::success(std::move(arguments));
}

std::optional<std::string> column_text_optional(sqlite3_stmt* const stmt, const int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, index));
    const auto bytes = sqlite3_column_bytes(stmt, index);
    if (text == nullptr || bytes <= 0) {
        return std::string{};
    }
    return std::string(text, static_cast<std::size_t>(bytes));
}

std::string column_text_required(sqlite3_stmt* const stmt, const int index) {
    auto value = column_text_optional(stmt, index);
    return value ? std::move(*value) : std::string{};
}

std::uint64_t column_uint64(sqlite3_stmt* const stmt, const int index) {
    return static_cast<std::uint64_t>(sqlite3_column_int64(stmt, index));
}

std::optional<std::uint64_t> column_uint64_optional(sqlite3_stmt* const stmt, const int index) {
    if (sqlite3_column_type(stmt, index) == SQLITE_NULL) {
        return std::nullopt;
    }
    return column_uint64(stmt, index);
}

base::Result<void>
validate_repository_connection_record(const ports::RepositoryConnectionRecord& record) {
    if (!valid_stable_value(record.connection_id, kMaximumIdentifierBytes) ||
        !valid_text(record.display_name, kMaximumDisplayNameBytes) ||
        !valid_text(record.locator, kMaximumLocatorBytes) ||
        !known_repository_state(record.state) || !valid_wire_integer(record.created_utc_ms) ||
        !valid_wire_integer(record.updated_utc_ms) ||
        record.updated_utc_ms < record.created_utc_ms ||
        record.capabilities.size() > kMaximumCapabilities) {
        return invalid("repository connection record is invalid");
    }
    if (record.credential_ref && !valid_text(record.credential_ref->value, kMaximumLocatorBytes)) {
        return invalid("repository credential ref is invalid");
    }
    std::string_view previous;
    for (const auto& capability : record.capabilities) {
        if (!valid_stable_value(capability, kMaximumCapabilityBytes) ||
            (!previous.empty() && capability <= previous)) {
            return invalid("repository capabilities are invalid or unsorted");
        }
        previous = capability;
    }
    return base::Result<void>::success();
}

base::Result<void> validate_job_record(const ports::JobRecord& record) {
    if (!valid_stable_value(record.job_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.trace_id, kMaximumIdentifierBytes) ||
        !known_job_operation(record.operation) || !known_job_state(record.state) ||
        !contracts::is_known_content_kind(record.content_kind) ||
        !valid_wire_integer(record.created_utc_ms) ||
        !valid_optional_wire_integer(record.started_utc_ms) ||
        !valid_optional_wire_integer(record.completed_utc_ms) ||
        (record.started_utc_ms && *record.started_utc_ms < record.created_utc_ms) ||
        (record.completed_utc_ms &&
         (!record.started_utc_ms || *record.completed_utc_ms < *record.started_utc_ms)) ||
        ports::is_terminal_job_state(record.state) != record.completed_utc_ms.has_value() ||
        !valid_stable_value(record.message_code, kMaximumMessageCodeBytes) ||
        !valid_source_ids(record.source_ids, true) ||
        (record.repository_connection_id &&
         !valid_stable_value(*record.repository_connection_id, kMaximumIdentifierBytes)) ||
        (record.target_source_id &&
         !valid_stable_value(*record.target_source_id, kMaximumIdentifierBytes)) ||
        (record.backup_type && !known_backup_type(*record.backup_type)) ||
        (record.parent_recovery_point_id &&
         !valid_stable_value(*record.parent_recovery_point_id, kMaximumIdentifierBytes)) ||
        (record.preflight_token && (record.preflight_token->empty() ||
                                    record.preflight_token->size() > kMaximumTokenBytes)) ||
        (record.idempotency_key &&
         !valid_stable_value(*record.idempotency_key, kMaximumIdentifierBytes)) ||
        (record.result_message_code &&
         !valid_stable_value(*record.result_message_code, kMaximumMessageCodeBytes)) ||
        record.request_fingerprint.size() > kMaximumCommandFingerprintBytes) {
        return invalid("job record is invalid");
    }
    if (record.idempotency_key && record.request_fingerprint.empty()) {
        return invalid("job with idempotency key requires a request fingerprint");
    }
    if (record.state == contracts::ServiceJobState::kQueued && record.started_utc_ms) {
        return invalid("queued job cannot have started timestamp");
    }
    if ((record.state == contracts::ServiceJobState::kRunning ||
         record.state == contracts::ServiceJobState::kCancelling) &&
        !record.started_utc_ms) {
        return invalid("active non-queued job requires started timestamp");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_schedule_record(const ports::ScheduleRecord& record) {
    auto valid_trigger = contracts::validate_schedule_trigger(record.trigger);
    if (!valid_stable_value(record.schedule_id, kMaximumIdentifierBytes) ||
        !valid_text(record.display_name, kMaximumDisplayNameBytes) ||
        !contracts::is_known_content_kind(record.content_kind) ||
        !valid_stable_value(record.repository_connection_id, kMaximumIdentifierBytes) ||
        !known_backup_type(record.backup_type) || !valid_trigger ||
        !valid_optional_wire_integer(record.next_run_utc_ms) ||
        !base::is_canonical_uuid(record.backup_set_uuid) ||
        (record.last_recovery_point_id &&
         !base::is_canonical_uuid(*record.last_recovery_point_id)) ||
        !valid_wire_integer(record.created_utc_ms) || !valid_wire_integer(record.updated_utc_ms) ||
        record.updated_utc_ms < record.created_utc_ms) {
        return invalid("schedule record is invalid");
    }
    if (record.owner_sid.size() > kMaximumLocatorBytes) {
        return invalid("schedule owner identity is invalid");
    }
    if (record.content_kind == contracts::ContentKind::kVolumeSet) {
        if (!valid_source_ids(record.source_ids, false) || !record.file_selections.empty()) {
            return invalid("volume schedule sources are invalid");
        }
    } else {
        if (!record.source_ids.empty() || record.file_selections.empty() ||
            record.file_selections.size() > contracts::kMaximumFileSelections ||
            record.backup_type != contracts::BackupType::kFull) {
            return invalid("file schedule sources are invalid");
        }
        auto refs = contracts::validate_file_source_refs(record.file_selections);
        if (!refs) {
            return refs;
        }
    }
    if (record.trigger.timezone_id.size() > kMaximumTimezoneBytes) {
        return invalid("schedule timezone is invalid");
    }
    // archive_password_protected is dpapi-lm:<schedule_id>:<base64>; entropy binds to schedule_id.
    constexpr std::size_t kMaximumProtectedSecretBytes = 4'096 + 160;
    constexpr std::string_view kDpapiPrefix = "dpapi-lm:";
    if (record.encryption_enabled) {
        const auto expected_prefix =
            std::string(kDpapiPrefix) + record.schedule_id + std::string(":");
        if (record.archive_password_protected.size() <= expected_prefix.size() ||
            record.archive_password_protected.size() > kMaximumProtectedSecretBytes ||
            !record.archive_password_protected.starts_with(expected_prefix)) {
            return invalid("encrypted schedule requires a protected password");
        }
    } else if (!record.archive_password_protected.empty()) {
        return invalid("unencrypted schedule cannot store a protected password");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_audit_event_record(const ports::AuditEventRecord& record) {
    auto valid_arguments = contracts::validate_message_arguments(record.message_arguments);
    if (!valid_stable_value(record.event_id, kMaximumIdentifierBytes) ||
        !valid_wire_integer(record.created_utc_ms) || !known_audit_severity(record.severity) ||
        !valid_stable_value(record.message_code, kMaximumMessageCodeBytes) || !valid_arguments ||
        !valid_stable_value(record.correlation_id, kMaximumIdentifierBytes) ||
        record.message_arguments.size() > kMaximumMessageArguments) {
        return invalid("audit event record is invalid");
    }
    for (const auto& argument : record.message_arguments) {
        if (!valid_text(argument.value, kMaximumMessageArgumentBytes)) {
            return invalid("audit event argument is invalid");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_command_record(const ports::CommandRecord& record) {
    if (!valid_stable_value(record.idempotency_key, kMaximumIdentifierBytes) ||
        !valid_text(record.request_fingerprint, kMaximumCommandFingerprintBytes) ||
        !valid_stable_value(record.command_id, kMaximumIdentifierBytes) ||
        (record.resource_id && !valid_stable_value(*record.resource_id, kMaximumIdentifierBytes)) ||
        !valid_wire_integer(record.created_utc_ms)) {
        return invalid("command record is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_restore_preflight_record(const ports::RestorePreflightRecord& record) {
    // chain_fingerprint is an opaque binding string (disk/volume/file restore prefixes).
    // Validate as printable text, not a stable identifier.
    if (!valid_text(record.preflight_token, kMaximumTokenBytes) ||
        !valid_stable_value(record.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.repository_uuid, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.recovery_point_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.target_source_id, kMaximumIdentifierBytes) ||
        !valid_text(record.chain_fingerprint, kMaximumCommandFingerprintBytes) ||
        !valid_wire_integer(record.logical_size_bytes) ||
        record.target_capacity_bytes < record.logical_size_bytes ||
        !valid_wire_integer(record.target_capacity_bytes) || record.chain_depth == 0 ||
        !valid_wire_integer(record.created_utc_ms) || !valid_wire_integer(record.expires_utc_ms) ||
        record.expires_utc_ms <= record.created_utc_ms) {
        return invalid("restore preflight record is invalid");
    }
    const bool is_file = record.chain_fingerprint.starts_with("filec|");
    if (is_file) {
        if (record.entry_ids.empty() ||
            record.entry_ids.size() > contracts::kMaximumFileRestoreEntryIds) {
            return invalid("file restore preflight entry_ids are invalid");
        }
        std::set<std::string_view> seen;
        for (const auto& entry_id : record.entry_ids) {
            if (entry_id.empty() || entry_id.size() > 20 || !seen.insert(entry_id).second) {
                return invalid("file restore preflight entry_ids are invalid");
            }
        }
    } else if (!record.entry_ids.empty() || record.logical_size_bytes == 0) {
        return invalid("volume restore preflight record is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_job_transition(const ports::JobStateTransition& transition) {
    if (!valid_stable_value(transition.job_id, kMaximumIdentifierBytes) ||
        !known_job_state(transition.expected_state) || !known_job_state(transition.next_state) ||
        !valid_wire_integer(transition.transition_utc_ms) ||
        !valid_stable_value(transition.message_code, kMaximumMessageCodeBytes) ||
        (transition.result_message_code &&
         !valid_stable_value(*transition.result_message_code, kMaximumMessageCodeBytes))) {
        return invalid("job transition is invalid");
    }
    if (!ports::is_valid_job_state_transition(transition.expected_state, transition.next_state)) {
        return invalid("job state transition is not allowed");
    }
    if (ports::is_terminal_job_state(transition.next_state) && transition.result_message_code &&
        transition.result_message_code->empty()) {
        return invalid("terminal job transition result message is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> exec_sql(sqlite3* const db, const char* const sql) {
    char* error_message = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &error_message);
    if (error_message != nullptr) {
        sqlite3_free(error_message);
    }
    return map_sqlite_result(rc, db, "exec sql failed");
}

base::Result<std::uint32_t> read_schema_version(sqlite3* const db) {
    auto statement = SqliteStatement::prepare(db, "SELECT version FROM schema_meta WHERE id = 1");
    if (!statement) {
        return base::Result<std::uint32_t>::failure(statement.error());
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<std::uint32_t>::failure(stepped.error());
    }
    if (stepped.value() == SQLITE_DONE) {
        return base::Result<std::uint32_t>::success(0);
    }
    const auto version = sqlite3_column_int64(statement.value().get(), 0);
    if (version <= 0 ||
        version > static_cast<std::int64_t>((std::numeric_limits<std::uint32_t>::max)())) {
        return base::Result<std::uint32_t>::failure(
            make_error(base::ErrorCode::kCorruptData, "schema version is corrupt"));
    }
    return base::Result<std::uint32_t>::success(static_cast<std::uint32_t>(version));
}

base::Result<void> write_schema_version(sqlite3* const db, const std::uint32_t version) {
    auto statement =
        SqliteStatement::prepare(db, "INSERT INTO schema_meta(id, version) VALUES(1, ?) "
                                     "ON CONFLICT(id) DO UPDATE SET version = excluded.version");
    if (!statement) {
        return base::Result<void>::failure(statement.error());
    }
    auto bound = statement.value().bind_int64(1, static_cast<std::int64_t>(version));
    if (!bound) {
        return bound;
    }
    auto stepped = statement.value().step();
    if (!stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    return base::Result<void>::success();
}

base::Result<ports::RepositoryConnectionRecord>
read_repository_connection(sqlite3_stmt* const stmt) {
    ports::RepositoryConnectionRecord record;
    record.connection_id = column_text_required(stmt, 0);
    record.display_name = column_text_required(stmt, 1);
    record.locator = column_text_required(stmt, 2);
    auto credential = column_text_optional(stmt, 3);
    if (credential) {
        record.credential_ref = contracts::SecretRef{std::move(*credential)};
    }
    record.state = static_cast<contracts::RepositoryConnectionState>(sqlite3_column_int(stmt, 4));
    record.is_default = sqlite3_column_int(stmt, 5) != 0;
    auto capabilities = decode_string_list(column_text_required(stmt, 6));
    if (!capabilities) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(capabilities.error());
    }
    record.capabilities = std::move(capabilities.value());
    record.created_utc_ms = column_uint64(stmt, 7);
    record.updated_utc_ms = column_uint64(stmt, 8);
    auto valid = validate_repository_connection_record(record);
    if (!valid) {
        return base::Result<ports::RepositoryConnectionRecord>::failure(valid.error());
    }
    return base::Result<ports::RepositoryConnectionRecord>::success(std::move(record));
}

base::Result<ports::JobRecord> read_job(sqlite3_stmt* const stmt) {
    ports::JobRecord record;
    record.job_id = column_text_required(stmt, 0);
    record.trace_id = column_text_required(stmt, 1);
    record.operation = static_cast<contracts::JobOperation>(sqlite3_column_int(stmt, 2));
    record.state = static_cast<contracts::ServiceJobState>(sqlite3_column_int(stmt, 3));
    record.content_kind = static_cast<contracts::ContentKind>(sqlite3_column_int(stmt, 4));
    record.created_utc_ms = column_uint64(stmt, 5);
    record.started_utc_ms = column_uint64_optional(stmt, 6);
    record.completed_utc_ms = column_uint64_optional(stmt, 7);
    auto source_ids = decode_string_list(column_text_required(stmt, 8));
    if (!source_ids) {
        return base::Result<ports::JobRecord>::failure(source_ids.error());
    }
    record.source_ids = std::move(source_ids).value();
    record.repository_connection_id = column_text_optional(stmt, 9);
    record.target_source_id = column_text_optional(stmt, 10);
    if (sqlite3_column_type(stmt, 11) != SQLITE_NULL) {
        record.backup_type = static_cast<contracts::BackupType>(sqlite3_column_int(stmt, 11));
    }
    record.parent_recovery_point_id = column_text_optional(stmt, 12);
    record.preflight_token = column_text_optional(stmt, 13);
    record.message_code = column_text_required(stmt, 14);
    record.idempotency_key = column_text_optional(stmt, 15);
    if (sqlite3_column_type(stmt, 16) != SQLITE_NULL) {
        record.result_error_code = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 16));
    }
    if (sqlite3_column_type(stmt, 17) != SQLITE_NULL) {
        record.result_outcome = static_cast<std::uint32_t>(sqlite3_column_int64(stmt, 17));
    }
    record.result_message_code = column_text_optional(stmt, 18);
    if (sqlite3_column_type(stmt, 19) != SQLITE_NULL) {
        record.exclude_page_and_hibernation_files = sqlite3_column_int(stmt, 19) != 0;
    }
    record.request_fingerprint = column_text_required(stmt, 20);
    auto valid = validate_job_record(record);
    if (!valid) {
        return base::Result<ports::JobRecord>::failure(valid.error());
    }
    return base::Result<ports::JobRecord>::success(std::move(record));
}

base::Result<ports::ScheduleRecord> read_schedule(sqlite3_stmt* const stmt) {
    ports::ScheduleRecord record;
    record.schedule_id = column_text_required(stmt, 0);
    record.display_name = column_text_required(stmt, 1);
    record.enabled = sqlite3_column_int(stmt, 2) != 0;
    record.content_kind = static_cast<contracts::ContentKind>(sqlite3_column_int(stmt, 3));
    auto source_ids = decode_string_list(column_text_required(stmt, 4));
    if (!source_ids) {
        return base::Result<ports::ScheduleRecord>::failure(source_ids.error());
    }
    record.source_ids = std::move(source_ids).value();
    record.owner_sid = column_text_required(stmt, 5);
    record.repository_connection_id = column_text_required(stmt, 6);
    record.backup_type = static_cast<contracts::BackupType>(sqlite3_column_int(stmt, 7));
    record.trigger.kind = static_cast<contracts::ScheduleTriggerKind>(sqlite3_column_int(stmt, 8));
    record.trigger.local_minute_of_day = static_cast<std::uint16_t>(sqlite3_column_int(stmt, 9));
    record.trigger.weekday_mask = static_cast<std::uint8_t>(sqlite3_column_int(stmt, 10));
    record.trigger.timezone_id = column_text_required(stmt, 11);
    record.next_run_utc_ms = column_uint64_optional(stmt, 12);
    record.exclude_page_and_hibernation_files = sqlite3_column_int(stmt, 13) != 0;
    record.encryption_enabled = sqlite3_column_int(stmt, 14) != 0;
    record.archive_password_protected = column_text_required(stmt, 15);
    record.backup_set_uuid = column_text_required(stmt, 16);
    record.last_recovery_point_id = column_text_optional(stmt, 17);
    record.created_utc_ms = column_uint64(stmt, 18);
    record.updated_utc_ms = column_uint64(stmt, 19);
    // file_selections loaded by schedule store after read when content_kind is file_set.
    auto valid = validate_schedule_record(record);
    if (!valid && record.content_kind == contracts::ContentKind::kFileSet &&
        record.file_selections.empty()) {
        // Defer full validation until selections are attached by the store.
        return base::Result<ports::ScheduleRecord>::success(std::move(record));
    }
    if (!valid) {
        return base::Result<ports::ScheduleRecord>::failure(valid.error());
    }
    return base::Result<ports::ScheduleRecord>::success(std::move(record));
}

base::Result<ports::AuditEventRecord> read_audit_event(sqlite3_stmt* const stmt) {
    ports::AuditEventRecord record;
    record.event_id = column_text_required(stmt, 0);
    record.created_utc_ms = column_uint64(stmt, 1);
    record.severity = static_cast<contracts::AuditSeverity>(sqlite3_column_int(stmt, 2));
    record.message_code = column_text_required(stmt, 3);
    auto arguments = decode_message_arguments(column_text_required(stmt, 4));
    if (!arguments) {
        return base::Result<ports::AuditEventRecord>::failure(arguments.error());
    }
    record.message_arguments = std::move(arguments.value());
    record.correlation_id = column_text_required(stmt, 5);
    auto valid = validate_audit_event_record(record);
    if (!valid) {
        return base::Result<ports::AuditEventRecord>::failure(valid.error());
    }
    return base::Result<ports::AuditEventRecord>::success(std::move(record));
}

base::Result<ports::CommandRecord> read_command(sqlite3_stmt* const stmt) {
    ports::CommandRecord record;
    record.idempotency_key = column_text_required(stmt, 0);
    record.request_fingerprint = column_text_required(stmt, 1);
    record.command_id = column_text_required(stmt, 2);
    record.resource_id = column_text_optional(stmt, 3);
    record.created_utc_ms = column_uint64(stmt, 4);
    auto valid = validate_command_record(record);
    return valid ? base::Result<ports::CommandRecord>::success(std::move(record))
                 : base::Result<ports::CommandRecord>::failure(valid.error());
}

base::Result<ports::RestorePreflightRecord> read_restore_preflight(sqlite3_stmt* const stmt) {
    ports::RestorePreflightRecord record;
    record.preflight_token = column_text_required(stmt, 0);
    record.repository_connection_id = column_text_required(stmt, 1);
    record.repository_uuid = column_text_required(stmt, 2);
    record.recovery_point_id = column_text_required(stmt, 3);
    record.target_source_id = column_text_required(stmt, 4);
    record.chain_fingerprint = column_text_required(stmt, 5);
    record.logical_size_bytes = column_uint64(stmt, 6);
    record.target_capacity_bytes = column_uint64(stmt, 7);
    const auto chain_depth = column_uint64(stmt, 8);
    if (chain_depth > (std::numeric_limits<std::uint32_t>::max)()) {
        return base::Result<ports::RestorePreflightRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "restore preflight chain depth is corrupt"));
    }
    record.chain_depth = static_cast<std::uint32_t>(chain_depth);
    record.created_utc_ms = column_uint64(stmt, 9);
    record.expires_utc_ms = column_uint64(stmt, 10);
    // entry_ids are attached by the store after this read; validate base fields only here.
    if (!valid_text(record.preflight_token, kMaximumTokenBytes) ||
        !valid_stable_value(record.repository_connection_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.repository_uuid, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.recovery_point_id, kMaximumIdentifierBytes) ||
        !valid_stable_value(record.target_source_id, kMaximumIdentifierBytes) ||
        !valid_text(record.chain_fingerprint, kMaximumCommandFingerprintBytes) ||
        !valid_wire_integer(record.logical_size_bytes) ||
        record.target_capacity_bytes < record.logical_size_bytes ||
        !valid_wire_integer(record.target_capacity_bytes) || record.chain_depth == 0 ||
        !valid_wire_integer(record.created_utc_ms) || !valid_wire_integer(record.expires_utc_ms) ||
        record.expires_utc_ms <= record.created_utc_ms) {
        return base::Result<ports::RestorePreflightRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "restore preflight record is invalid"));
    }
    if (!record.chain_fingerprint.starts_with("filec|") && record.logical_size_bytes == 0) {
        return base::Result<ports::RestorePreflightRecord>::failure(
            make_error(base::ErrorCode::kCorruptData, "restore preflight record is invalid"));
    }
    return base::Result<ports::RestorePreflightRecord>::success(std::move(record));
}

contracts::RepositoryConnectionSummary
to_connection_summary(const ports::RepositoryConnectionRecord& record) {
    return {record.connection_id, record.display_name, record.state, record.is_default,
            record.capabilities};
}

contracts::JobSummary to_job_summary(const ports::JobRecord& record) {
    contracts::JobSummary summary;
    summary.job_id = record.job_id;
    summary.trace_id = record.trace_id;
    summary.operation = record.operation;
    summary.state = record.state;
    if (record.operation == contracts::JobOperation::kBackup ||
        record.operation == contracts::JobOperation::kRestore ||
        record.operation == contracts::JobOperation::kVerify) {
        summary.content_kind = record.content_kind;
    }
    summary.created_utc_ms = record.created_utc_ms;
    summary.started_utc_ms = record.started_utc_ms;
    summary.completed_utc_ms = record.completed_utc_ms;
    summary.message_code = record.message_code;
    summary.source_ids = record.source_ids;
    summary.repository_connection_id = record.repository_connection_id;
    return summary;
}

contracts::ScheduleSummary to_schedule_summary(const ports::ScheduleRecord& record) {
    contracts::ScheduleSummary summary;
    summary.schedule_id = record.schedule_id;
    summary.display_name = record.display_name;
    summary.enabled = record.enabled;
    summary.content_kind = record.content_kind;
    if (record.content_kind == contracts::ContentKind::kVolumeSet) {
        summary.source_ids = record.source_ids;
    } else {
        summary.selection_summaries.reserve(record.file_selections.size());
        for (const auto& selection : record.file_selections) {
            contracts::FileSelectionSummary item;
            item.selection_id = selection.selection_id;
            item.display_label = selection.display_label;
            item.entry_kind = selection.entry_kind;
            item.recursion = selection.recursion;
            summary.selection_summaries.push_back(std::move(item));
        }
    }
    summary.repository_connection_id = record.repository_connection_id;
    summary.backup_type = record.backup_type;
    summary.trigger = record.trigger;
    summary.next_run_utc_ms = record.next_run_utc_ms;
    summary.exclude_page_and_hibernation_files = record.exclude_page_and_hibernation_files;
    summary.encryption_enabled = record.encryption_enabled;
    return summary;
}

namespace {

[[nodiscard]] char path_hex_digit(const unsigned value) noexcept {
    return static_cast<char>(value < 10 ? '0' + value : 'a' + (value - 10));
}

[[nodiscard]] int path_hex_value(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

} // namespace

std::string encode_relative_path_blob(const std::vector<contracts::EncodedName>& components) {
    std::string encoded = "v1";
    for (const auto& component : components) {
        encoded.push_back('|');
        encoded += std::to_string(static_cast<unsigned>(component.encoding));
        encoded.push_back(':');
        for (const auto byte : component.bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            encoded.push_back(path_hex_digit((value >> 4U) & 0x0FU));
            encoded.push_back(path_hex_digit(value & 0x0FU));
        }
    }
    return encoded;
}

base::Result<std::vector<contracts::EncodedName>>
decode_relative_path_blob(const std::string_view encoded) {
    if (!encoded.starts_with("v1")) {
        return base::Result<std::vector<contracts::EncodedName>>::failure(
            make_error(base::ErrorCode::kCorruptData, "relative path blob version is unsupported"));
    }
    std::vector<contracts::EncodedName> components;
    std::size_t index = 2;
    while (index < encoded.size()) {
        if (encoded[index] != '|') {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                make_error(base::ErrorCode::kCorruptData, "relative path blob is corrupt"));
        }
        ++index;
        const std::size_t colon = encoded.find(':', index);
        if (colon == std::string_view::npos || colon == index) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                make_error(base::ErrorCode::kCorruptData, "relative path blob is corrupt"));
        }
        unsigned encoding = 0;
        for (std::size_t cursor = index; cursor < colon; ++cursor) {
            if (encoded[cursor] < '0' || encoded[cursor] > '9') {
                return base::Result<std::vector<contracts::EncodedName>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "relative path blob is corrupt"));
            }
            encoding = encoding * 10U + static_cast<unsigned>(encoded[cursor] - '0');
        }
        index = colon + 1;
        std::size_t next = encoded.find('|', index);
        if (next == std::string_view::npos) {
            next = encoded.size();
        }
        const auto hex = encoded.substr(index, next - index);
        if ((hex.size() % 2U) != 0U) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                make_error(base::ErrorCode::kCorruptData, "relative path blob is corrupt"));
        }
        contracts::EncodedName name;
        name.encoding = static_cast<contracts::NameEncoding>(encoding);
        name.bytes.reserve(hex.size() / 2U);
        for (std::size_t byte_index = 0; byte_index < hex.size(); byte_index += 2) {
            const int high = path_hex_value(hex[byte_index]);
            const int low = path_hex_value(hex[byte_index + 1]);
            if (high < 0 || low < 0) {
                return base::Result<std::vector<contracts::EncodedName>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "relative path blob is corrupt"));
            }
            name.bytes.push_back(static_cast<std::byte>((high << 4) | low));
        }
        components.push_back(std::move(name));
        index = next;
    }
    return base::Result<std::vector<contracts::EncodedName>>::success(std::move(components));
}

base::Result<void>
replace_schedule_file_selections(sqlite3* const db, const std::string_view schedule_id,
                                 const std::vector<contracts::FileSourceRef>& selections) {
    auto clear = SqliteStatement::prepare(db,
                                          "DELETE FROM schedule_file_selections WHERE schedule_id = ?");
    if (!clear) {
        return base::Result<void>::failure(clear.error());
    }
    if (auto bound = clear.value().bind_text(1, schedule_id); !bound) {
        return bound;
    }
    if (auto stepped = clear.value().step(); !stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    auto insert = SqliteStatement::prepare(
        db,
        "INSERT INTO schedule_file_selections(schedule_id, ordinal, selection_id, volume_identity, "
        "relative_path_blob, entry_kind, recursion, reparse_policy, unreadable_policy, "
        "display_label) VALUES(?,?,?,?,?,?,?,?,?,?)");
    if (!insert) {
        return base::Result<void>::failure(insert.error());
    }
    for (std::size_t ordinal = 0; ordinal < selections.size(); ++ordinal) {
        const auto& selection = selections[ordinal];
        if (auto reset = insert.value().reset(); !reset) {
            return reset;
        }
        if (auto bound = insert.value().bind_text(1, schedule_id); !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_int64(2, static_cast<std::int64_t>(ordinal)); !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_text(3, selection.selection_id); !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_text(4, selection.volume_identity); !bound) {
            return bound;
        }
        if (auto bound =
                insert.value().bind_text(5, encode_relative_path_blob(selection.relative_components));
            !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_int64(6, static_cast<std::int64_t>(selection.entry_kind));
            !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_int64(7, static_cast<std::int64_t>(selection.recursion));
            !bound) {
            return bound;
        }
        if (auto bound =
                insert.value().bind_int64(8, static_cast<std::int64_t>(selection.reparse_policy));
            !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_int64(
                9, static_cast<std::int64_t>(selection.unreadable_policy));
            !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_text(10, selection.display_label); !bound) {
            return bound;
        }
        if (auto stepped = insert.value().step(); !stepped) {
            return base::Result<void>::failure(stepped.error());
        }
    }
    return base::Result<void>::success();
}

base::Result<std::vector<contracts::FileSourceRef>>
load_schedule_file_selections(sqlite3* const db, const std::string_view schedule_id) {
    auto statement = SqliteStatement::prepare(
        db,
        "SELECT selection_id, volume_identity, relative_path_blob, entry_kind, recursion, "
        "reparse_policy, unreadable_policy, display_label FROM schedule_file_selections "
        "WHERE schedule_id = ? ORDER BY ordinal ASC");
    if (!statement) {
        return base::Result<std::vector<contracts::FileSourceRef>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, schedule_id); !bound) {
        return base::Result<std::vector<contracts::FileSourceRef>>::failure(bound.error());
    }
    std::vector<contracts::FileSourceRef> selections;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<std::vector<contracts::FileSourceRef>>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        contracts::FileSourceRef ref;
        ref.selection_id = column_text_required(statement.value().get(), 0);
        ref.volume_identity = column_text_required(statement.value().get(), 1);
        auto components = decode_relative_path_blob(column_text_required(statement.value().get(), 2));
        if (!components) {
            return base::Result<std::vector<contracts::FileSourceRef>>::failure(components.error());
        }
        ref.relative_components = std::move(components).value();
        ref.entry_kind =
            static_cast<contracts::FileEntryKind>(sqlite3_column_int(statement.value().get(), 3));
        ref.recursion =
            static_cast<contracts::FileRecursion>(sqlite3_column_int(statement.value().get(), 4));
        ref.reparse_policy =
            static_cast<contracts::FileReparsePolicy>(sqlite3_column_int(statement.value().get(), 5));
        ref.unreadable_policy = static_cast<contracts::FileUnreadablePolicy>(
            sqlite3_column_int(statement.value().get(), 6));
        ref.display_label = column_text_required(statement.value().get(), 7);
        selections.push_back(std::move(ref));
    }
    return base::Result<std::vector<contracts::FileSourceRef>>::success(std::move(selections));
}

base::Result<void>
replace_restore_preflight_entry_ids(sqlite3* const db, const std::string_view preflight_token,
                                    const std::vector<std::string>& entry_ids) {
    auto clear = SqliteStatement::prepare(
        db, "DELETE FROM restore_preflight_entry_ids WHERE preflight_token = ?");
    if (!clear) {
        return base::Result<void>::failure(clear.error());
    }
    if (auto bound = clear.value().bind_text(1, preflight_token); !bound) {
        return bound;
    }
    if (auto stepped = clear.value().step(); !stepped) {
        return base::Result<void>::failure(stepped.error());
    }
    if (entry_ids.empty()) {
        return base::Result<void>::success();
    }
    auto insert = SqliteStatement::prepare(
        db,
        "INSERT INTO restore_preflight_entry_ids(preflight_token, ordinal, entry_id) VALUES(?,?,?)");
    if (!insert) {
        return base::Result<void>::failure(insert.error());
    }
    for (std::size_t ordinal = 0; ordinal < entry_ids.size(); ++ordinal) {
        if (auto reset = insert.value().reset(); !reset) {
            return reset;
        }
        if (auto bound = insert.value().bind_text(1, preflight_token); !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_int64(2, static_cast<std::int64_t>(ordinal)); !bound) {
            return bound;
        }
        if (auto bound = insert.value().bind_text(3, entry_ids[ordinal]); !bound) {
            return bound;
        }
        if (auto stepped = insert.value().step(); !stepped) {
            return base::Result<void>::failure(stepped.error());
        }
    }
    return base::Result<void>::success();
}

base::Result<std::vector<std::string>>
load_restore_preflight_entry_ids(sqlite3* const db, const std::string_view preflight_token) {
    auto statement = SqliteStatement::prepare(
        db, "SELECT entry_id FROM restore_preflight_entry_ids WHERE preflight_token = ? "
            "ORDER BY ordinal ASC");
    if (!statement) {
        return base::Result<std::vector<std::string>>::failure(statement.error());
    }
    if (auto bound = statement.value().bind_text(1, preflight_token); !bound) {
        return base::Result<std::vector<std::string>>::failure(bound.error());
    }
    std::vector<std::string> entry_ids;
    while (true) {
        auto stepped = statement.value().step();
        if (!stepped) {
            return base::Result<std::vector<std::string>>::failure(stepped.error());
        }
        if (stepped.value() == SQLITE_DONE) {
            break;
        }
        entry_ids.push_back(column_text_required(statement.value().get(), 0));
    }
    return base::Result<std::vector<std::string>>::success(std::move(entry_ids));
}

contracts::AuditEventSummary to_audit_summary(const ports::AuditEventRecord& record) {
    return {record.event_id,     record.created_utc_ms,    record.severity,
            record.message_code, record.message_arguments, record.correlation_id};
}

} // namespace aegra::adapters::sqlite::detail
