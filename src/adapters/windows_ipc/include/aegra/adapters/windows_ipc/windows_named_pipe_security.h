#pragma once

#include <cstdint>

namespace aegra::adapters::windows_ipc {

// Named-pipe ACL selection. Session identity and caller authorization are intentionally absent.
enum class WindowsNamedPipeAclProfile : std::uint8_t {
    // Process-token default DACL + PIPE_REJECT_REMOTE_CLIENTS.
    // Use for private Worker parent-child pipes.
    kProcessDefault = 1,
    // Explicit local-control DACL: LocalSystem + Administrators full; Everyone read/write.
    // PIPE_REJECT_REMOTE_CLIENTS still restricts the listener to the local machine.
    kLocalEveryoneControl = 2,
};

} // namespace aegra::adapters::windows_ipc
