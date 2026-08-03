#pragma once

#include "aegra/base/result.h"

#include <cstdint>
#include <string>

namespace aegra::adapters::windows_ipc {

// Local Service control-pipe authorization. Contracts and JSON stay out of this adapter.
enum class WindowsNamedPipeAclProfile : std::uint8_t {
    // Process-token default DACL + PIPE_REJECT_REMOTE_CLIENTS.
    // Use for interactive/dev processes where the service runs as the same user as Desktop.
    kProcessDefault = 1,
    // Explicit LocalSystem-oriented DACL: LocalSystem + Administrators full; Interactive read/write.
    // Intended only when the listener runs as LocalSystem; not reliable for ordinary user processes.
    kServiceLocalControl = 2,
};

enum class WindowsServiceCallerPolicy : std::uint8_t {
    // Any local peer from an interactive session (session > 0) or Builtin Administrators.
    kLocalInteractiveOrAdmin = 1,
};

struct WindowsNamedPipePeerIdentity final {
    std::string user_sid;
    std::uint32_t session_id{0};
    std::uint32_t process_id{0};
    bool is_local{true};
    bool is_interactive{false};
    bool is_administrator{false};
};

struct WindowsServiceCallerAuthorization final {
    WindowsServiceCallerPolicy policy{WindowsServiceCallerPolicy::kLocalInteractiveOrAdmin};
};

[[nodiscard]] base::Result<void>
authorize_service_caller(const WindowsNamedPipePeerIdentity& peer,
                         const WindowsServiceCallerAuthorization& authorization);

} // namespace aegra::adapters::windows_ipc
