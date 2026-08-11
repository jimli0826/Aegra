#include "sqlite_internal.h"

#include <array>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

namespace aegra::adapters::sqlite::detail {
namespace {

constexpr std::size_t kMaximumTokenBytes = 1'024;

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
                make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
        }
        const int high = hex_value(value[index + 1]);
        const int low = hex_value(value[index + 2]);
        if (high < 0 || low < 0) {
            return base::Result<std::string>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
        }
        decoded.push_back(static_cast<char>((high << 4) | low));
        index += 2;
    }
    return base::Result<std::string>::success(std::move(decoded));
}

[[nodiscard]] std::string optional_enum_filter(const char* key,
                                               const std::optional<std::uint32_t>& value) {
    if (!value) {
        return std::string(key) + "=*";
    }
    return std::string(key) + "=" + std::to_string(*value);
}

[[nodiscard]] std::string optional_u64_filter(const char* key,
                                              const std::optional<std::uint64_t>& value) {
    if (!value) {
        return std::string(key) + "=*";
    }
    return std::string(key) + "=" + std::to_string(*value);
}

[[nodiscard]] base::Result<std::array<std::string_view, 5>>
split_page_token(const std::string_view token) {
    std::array<std::string_view, 5> parts{};
    std::size_t begin = 0;
    for (std::size_t part_index = 0; part_index < 4; ++part_index) {
        const auto end = token.find('|', begin);
        if (end == std::string_view::npos) {
            return base::Result<std::array<std::string_view, 5>>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
        }
        parts[part_index] = token.substr(begin, end - begin);
        begin = end + 1;
    }
    parts[4] = token.substr(begin);
    if (parts[0].empty() || parts[1].empty() || parts[3].empty() || parts[4].empty()) {
        return base::Result<std::array<std::string_view, 5>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
    }
    return base::Result<std::array<std::string_view, 5>>::success(parts);
}

} // namespace

std::string repository_connection_page_filter(
    const std::optional<contracts::RepositoryConnectionState>& state) {
    if (!state) {
        return "s=*";
    }
    return "s=" + std::to_string(static_cast<std::uint32_t>(*state));
}

std::string job_page_filter(const contracts::JobListRequest& request) {
    const std::optional<std::uint32_t> op =
        request.operation
            ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*request.operation)}
            : std::nullopt;
    const std::optional<std::uint32_t> st =
        request.state ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*request.state)}
                      : std::nullopt;
    std::string filter = optional_enum_filter("o", op) + "&" + optional_enum_filter("s", st);
    filter += "&sc=" + std::to_string(static_cast<std::uint32_t>(request.scope));
    filter +=
        "&f=" + (request.from_utc_ms ? std::to_string(*request.from_utc_ms) : std::string{"*"});
    filter += "&t=" + (request.to_utc_ms ? std::to_string(*request.to_utc_ms) : std::string{"*"});
    return filter;
}

std::string schedule_page_filter(const std::optional<bool>& enabled) {
    if (!enabled) {
        return "e=*";
    }
    return *enabled ? "e=1" : "e=0";
}

std::string
audit_event_page_filter(const std::optional<contracts::AuditSeverity>& minimum_severity,
                        const std::optional<std::uint64_t>& from_utc_ms,
                        const std::optional<std::uint64_t>& to_utc_ms,
                        const std::optional<std::string>& correlation_id) {
    const std::optional<std::uint32_t> severity =
        minimum_severity
            ? std::optional<std::uint32_t>{static_cast<std::uint32_t>(*minimum_severity)}
            : std::nullopt;
    std::string filter = optional_enum_filter("v", severity) + "&" +
                         optional_u64_filter("f", from_utc_ms) + "&" +
                         optional_u64_filter("t", to_utc_ms) + "&c=";
    if (!correlation_id) {
        filter += "*";
    } else {
        filter += percent_encode(*correlation_id);
    }
    return filter;
}

base::Result<std::optional<PageCursor>>
decode_page_token(const std::optional<std::string>& token, const std::string_view expected_scope,
                  const std::string_view expected_filter) {
    if (!token) {
        return base::Result<std::optional<PageCursor>>::success(std::nullopt);
    }
    if (token->empty() || token->size() > kMaximumTokenBytes) {
        return base::Result<std::optional<PageCursor>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
    }
    auto parts = split_page_token(*token);
    if (!parts) {
        return base::Result<std::optional<PageCursor>>::failure(parts.error());
    }
    if (parts.value()[0] != "v1" || parts.value()[1] != expected_scope) {
        return base::Result<std::optional<PageCursor>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token scope mismatch"));
    }
    auto filter = percent_decode(parts.value()[2]);
    if (!filter || filter.value() != expected_filter) {
        return base::Result<std::optional<PageCursor>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token filter mismatch"));
    }
    std::uint64_t created = 0;
    const auto created_text = parts.value()[3];
    const auto* begin = created_text.data();
    const auto* end = begin + created_text.size();
    const auto parsed = std::from_chars(begin, end, created);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        created > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)())) {
        return base::Result<std::optional<PageCursor>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
    }
    auto id = percent_decode(parts.value()[4]);
    if (!id) {
        return base::Result<std::optional<PageCursor>>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "continuation token is invalid"));
    }
    return base::Result<std::optional<PageCursor>>::success(
        PageCursor{created, std::move(id.value())});
}

std::string encode_page_token(const std::string_view scope, const std::string_view filter,
                              const std::uint64_t created_utc_ms, const std::string_view id) {
    return std::string("v1|") + std::string(scope) + "|" + percent_encode(filter) + "|" +
           std::to_string(created_utc_ms) + "|" + percent_encode(id);
}

base::Result<void> clear_default_repository_flags(sqlite3* const db) {
    return exec_sql(db, "UPDATE repository_connections SET is_default = 0 WHERE is_default = 1");
}

base::Result<void> begin_savepoint(sqlite3* const db, const std::string_view name) {
    return exec_sql(db, ("SAVEPOINT " + std::string(name)).c_str());
}

base::Result<void> release_savepoint(sqlite3* const db, const std::string_view name) {
    return exec_sql(db, ("RELEASE " + std::string(name)).c_str());
}

void rollback_savepoint(sqlite3* const db, const std::string_view name) noexcept {
    (void)exec_sql(db, ("ROLLBACK TO " + std::string(name)).c_str());
    (void)exec_sql(db, ("RELEASE " + std::string(name)).c_str());
}

base::Result<void> bind_page_cursor(SqliteStatement& statement, int& index,
                                    const std::optional<PageCursor>& cursor) {
    if (!cursor) {
        return base::Result<void>::success();
    }
    if (auto bound =
            statement.bind_int64(index++, static_cast<std::int64_t>(cursor->created_utc_ms));
        !bound) {
        return bound;
    }
    if (auto bound =
            statement.bind_int64(index++, static_cast<std::int64_t>(cursor->created_utc_ms));
        !bound) {
        return bound;
    }
    return statement.bind_text(index++, cursor->id);
}

} // namespace aegra::adapters::sqlite::detail
