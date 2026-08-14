#include "aegra/application/file_browse_service.h"

#include "aegra/base/uuid.h"

#include <array>
#include <cstddef>
#include <utility>

namespace aegra::application {
namespace {

[[nodiscard]] std::uint64_t utc_ms(const ports::IClock& clock) {
    const auto now = clock.now_utc_ms();
    return now < 0 ? 0 : static_cast<std::uint64_t>(now);
}

[[nodiscard]] base::Result<std::string> random_token(ports::IRandomSource& random,
                                                     const base::CancellationToken& cancellation) {
    std::array<std::byte, 16> bytes{};
    if (auto filled = random.fill(bytes, cancellation); !filled) {
        return base::Result<std::string>::failure(filled.error());
    }
    constexpr char kHex[] = "0123456789abcdef";
    std::string token;
    token.reserve(32);
    for (const auto byte : bytes) {
        const auto value = std::to_integer<unsigned>(byte);
        token.push_back(kHex[value >> 4U]);
        token.push_back(kHex[value & 0x0FU]);
    }
    return base::Result<std::string>::success(std::move(token));
}

} // namespace

FileBrowseService::FileBrowseService(ports::IFileSourceBrowser& browser, ports::IClock& clock,
                                     ports::IRandomSource& random) noexcept
    : browser_(browser), clock_(clock), random_(random) {}

base::Result<std::string>
FileBrowseService::mint_token(const ports::FileBrowseSession& session, std::string adapter_token,
                              const base::CancellationToken cancellation) {
    auto token = random_token(random_, cancellation);
    if (!token) {
        return token;
    }
    const auto now = utc_ms(clock_);
    std::lock_guard lock(mutex_);
    purge_expired_unlocked(now);
    // L22: active tokens are limited per connection session, not process-wide.
    std::uint32_t session_active = 0;
    for (const auto& item : tokens_) {
        if (item.second.session_id != session.session_id) {
            continue;
        }
        ++session_active;
        if (session_active >= kMaximumActiveTokens) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kConflict, "file_browse.token_limit"});
        }
    }
    TokenEntry entry;
    entry.adapter_token = std::move(adapter_token);
    entry.session_id = session.session_id;
    entry.expires_utc_ms = now + kDefaultTokenTtlMs;
    tokens_.emplace(token.value(), std::move(entry));
    return token;
}

base::Result<FileBrowseService::TokenEntry>
FileBrowseService::lookup_token(const ports::FileBrowseSession& session,
                                const std::string& service_token) {
    const auto now = utc_ms(clock_);
    std::lock_guard lock(mutex_);
    purge_expired_unlocked(now);
    const auto found = tokens_.find(service_token);
    if (found == tokens_.end()) {
        return base::Result<TokenEntry>::failure(
            {base::ErrorCode::kNotFound, "file_browse.token_invalid"});
    }
    if (found->second.session_id != session.session_id) {
        return base::Result<TokenEntry>::failure(
            {base::ErrorCode::kNotFound, "file_browse.token_invalid"});
    }
    if (found->second.expires_utc_ms <= now) {
        tokens_.erase(found);
        return base::Result<TokenEntry>::failure(
            {base::ErrorCode::kNotFound, "file_browse.token_invalid"});
    }
    return base::Result<TokenEntry>::success(found->second);
}

void FileBrowseService::purge_expired_unlocked(const std::uint64_t now_utc_ms) noexcept {
    for (auto iterator = tokens_.begin(); iterator != tokens_.end();) {
        if (iterator->second.expires_utc_ms <= now_utc_ms) {
            iterator = tokens_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

void FileBrowseService::clear_session(const std::string_view session_id) noexcept {
    std::lock_guard lock(mutex_);
    for (auto iterator = tokens_.begin(); iterator != tokens_.end();) {
        if (iterator->second.session_id == session_id) {
            iterator = tokens_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

base::Result<contracts::FileSourceNodePage>
FileBrowseService::browse(const ports::FileBrowseSession& session,
                          const contracts::BrowseFileSourcesRequest& request,
                          const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_browse_file_sources_request(request); !valid) {
        return base::Result<contracts::FileSourceNodePage>::failure(valid.error());
    }
    if (session.session_id.empty()) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            {base::ErrorCode::kInvalidArgument, "file_browse.session_required"});
    }
    std::optional<std::string> adapter_parent;
    if (request.parent_node_token) {
        auto parent = lookup_token(session, *request.parent_node_token);
        if (!parent) {
            return base::Result<contracts::FileSourceNodePage>::failure(parent.error());
        }
        adapter_parent = parent.value().adapter_token;
    } else if (!request.page.continuation_token) {
        // First root page replaces the Desktop tree; drop prior session tokens so
        // collapse / re-open / locked hydrate cannot accumulate past L22. Continuation
        // pages keep earlier root tokens from this listing.
        clear_session(session.session_id);
    }
    auto page = browser_.list_children(session, adapter_parent, request.page,
                                       request.include_unavailable, cancellation);
    if (!page) {
        return page;
    }
    contracts::FileSourceNodePage remapped;
    remapped.continuation_token = page.value().continuation_token;
    remapped.items.reserve(page.value().items.size());
    for (auto& item : page.value().items) {
        auto service_token = mint_token(session, item.node_token, cancellation);
        if (!service_token) {
            return base::Result<contracts::FileSourceNodePage>::failure(service_token.error());
        }
        item.node_token = std::move(service_token).value();
        remapped.items.push_back(std::move(item));
    }
    return base::Result<contracts::FileSourceNodePage>::success(std::move(remapped));
}

base::Result<contracts::FileSourceRef> FileBrowseService::resolve_selection(
    const ports::FileBrowseSession& session, const std::string& node_token,
    const contracts::FileRecursion recursion, const std::string& display_label,
    const base::CancellationToken cancellation) {
    if (session.session_id.empty()) {
        return base::Result<contracts::FileSourceRef>::failure(
            {base::ErrorCode::kInvalidArgument, "file_browse.session_required"});
    }
    auto entry = lookup_token(session, node_token);
    if (!entry) {
        return base::Result<contracts::FileSourceRef>::failure(entry.error());
    }
    auto resolved = browser_.resolve_selection(session, entry.value().adapter_token, recursion,
                                               display_label, cancellation);
    if (!resolved) {
        return resolved;
    }
    std::array<std::byte, 16> bytes{};
    if (auto filled = random_.fill(bytes, cancellation); !filled) {
        return base::Result<contracts::FileSourceRef>::failure(filled.error());
    }
    bytes[6] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[6]) & 0x0FU) | 0x40U);
    bytes[8] = static_cast<std::byte>((std::to_integer<unsigned>(bytes[8]) & 0x3FU) | 0x80U);
    resolved.value().selection_id = base::format_uuid(bytes);
    auto valid = contracts::validate_file_source_ref(resolved.value());
    if (!valid) {
        return base::Result<contracts::FileSourceRef>::failure(valid.error());
    }
    return resolved;
}

} // namespace aegra::application
