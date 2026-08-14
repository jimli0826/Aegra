#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/service_control.h"

#include <optional>
#include <string>

namespace aegra::ports {

/// Per-pipe-session context for opaque token isolation. It carries no caller identity.
struct FileBrowseSession final {
    std::string session_id;
};

/// Service-facing browse port. Inputs/outputs use opaque node tokens only.
/// No platform paths, HANDLE, or Volume GUID are returned to callers.
/// Thread safety: implementation may be called from the Service request thread only unless
/// documented otherwise; token store concurrency is the Service's responsibility.
/// Cancel: enumerate must observe cancellation; token minting is local and fast.
class IFileSourceBrowser {
  public:
    IFileSourceBrowser() = default;
    virtual ~IFileSourceBrowser() = default;
    IFileSourceBrowser(const IFileSourceBrowser&) = delete;
    IFileSourceBrowser& operator=(const IFileSourceBrowser&) = delete;
    IFileSourceBrowser(IFileSourceBrowser&&) = delete;
    IFileSourceBrowser& operator=(IFileSourceBrowser&&) = delete;

    /// Lists roots (parent null) or children of parent_node_token.
    /// Tokens are opaque; validity and TTL are enforced by Service before/after this call.
    [[nodiscard]] virtual base::Result<contracts::FileSourceNodePage>
    list_children(const FileBrowseSession& session,
                  const std::optional<std::string>& parent_node_token,
                  const contracts::ServicePageRequest& page, bool include_unavailable,
                  base::CancellationToken cancellation) = 0;

    /// Resolves a still-valid node token to a durable FileSourceRef for schedule create.
    /// On failure returns NotFound/InvalidArgument without path text.
    [[nodiscard]] virtual base::Result<contracts::FileSourceRef>
    resolve_selection(const FileBrowseSession& session, const std::string& node_token,
                      contracts::FileRecursion recursion, const std::string& display_label,
                      base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
