#pragma once

#include "aegra/contracts/service.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::apps::service {

class IServiceLog;

namespace detail {

inline constexpr std::string_view kPlainLogEvent = "service.plain";

[[nodiscard]] std::string_view request_kind_name(contracts::ServiceRequestKind kind) noexcept;
[[nodiscard]] std::string request_log_detail(const contracts::ServiceRequest& request);
[[nodiscard]] std::string response_log_detail(const contracts::ServiceResponse& response);
[[nodiscard]] std::string readable_code(std::string_view code);
[[nodiscard]] std::string sanitized_interaction_detail(std::string_view direction,
                                                       std::string_view encoded);

[[nodiscard]] std::string format_log_bytes(std::uint64_t bytes);
[[nodiscard]] std::string format_log_duration(std::chrono::milliseconds elapsed);
[[nodiscard]] std::string pad_log_key(std::string_view key);

/// Backup-style plain lines for Service info.log (no event suffix).
class PlainFlowLog final {
  public:
    explicit PlainFlowLog(IServiceLog* logger) noexcept : logger_(logger) {}

    void line(std::string_view text) noexcept;
    void blank() noexcept;
    void section(std::string_view title) noexcept;
    void field(std::string_view key, std::string_view value) noexcept;
    void field_u64(std::string_view key, std::uint64_t value) noexcept;
    void field_bool(std::string_view key, bool value) noexcept;
    void field_bytes(std::string_view key, std::uint64_t bytes) noexcept;
    void stage_begin(std::string_view stage) noexcept;
    void stage_ok(std::string_view stage, std::chrono::milliseconds elapsed) noexcept;
    void stage_fail(std::string_view stage, std::chrono::milliseconds elapsed,
                    std::string_view error_code, std::string_view message_code) noexcept;

  private:
    IServiceLog* logger_;
};

} // namespace detail
} // namespace aegra::apps::service
