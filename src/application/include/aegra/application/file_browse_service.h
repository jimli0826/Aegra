#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/contracts/service_control.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/file_browser.h"
#include "aegra/ports/random.h"

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace aegra::application {

/// Service-facing browse use case: opaque token TTL, caller binding, and selection resolve.
/// Adapter tokens stay private; Desktop only sees Service-minted tokens.
class FileBrowseService final {
  public:
    static constexpr std::uint64_t kDefaultTokenTtlMs = 15ULL * 60ULL * 1000ULL;
    static constexpr std::uint32_t kMaximumActiveTokens = 4096;

    FileBrowseService(ports::IFileSourceBrowser& browser, ports::IClock& clock,
                      ports::IRandomSource& random) noexcept;

    [[nodiscard]] base::Result<contracts::FileSourceNodePage>
    browse(const ports::FileBrowseCaller& caller, const contracts::BrowseFileSourcesRequest& request,
           base::CancellationToken cancellation);

    [[nodiscard]] base::Result<contracts::FileSourceRef>
    resolve_selection(const ports::FileBrowseCaller& caller, const std::string& node_token,
                      contracts::FileRecursion recursion, const std::string& display_label,
                      base::CancellationToken cancellation);

    /// Drop all tokens for a disconnected pipe session.
    void clear_session(std::string_view session_id) noexcept;

  private:
    struct TokenEntry final {
        std::string adapter_token;
        std::string caller_sid;
        std::string session_id;
        std::uint64_t expires_utc_ms{0};
    };

    [[nodiscard]] base::Result<std::string>
    mint_token(const ports::FileBrowseCaller& caller, std::string adapter_token,
               base::CancellationToken cancellation);
    [[nodiscard]] base::Result<TokenEntry>
    lookup_token(const ports::FileBrowseCaller& caller, const std::string& service_token);
    void purge_expired_unlocked(std::uint64_t now_utc_ms) noexcept;

    ports::IFileSourceBrowser& browser_;
    ports::IClock& clock_;
    ports::IRandomSource& random_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, TokenEntry> tokens_;
};

} // namespace aegra::application
