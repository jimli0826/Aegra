#pragma once

#include <cstdint>

namespace aegra::desktop {

/// ServiceClient connection state (aliased as ServiceClient::State).
enum class ServiceClientState : std::uint8_t {
    kDisconnected,
    kConnecting,
    kReady,
};

/// Which consumer a ListJobs query serves (aliased as ServiceClient::JobQueryPurpose).
enum class ServiceClientJobQueryPurpose : std::uint8_t {
    kActive = 1,
    kTerminalSeed = 2,
    kTaskLog = 3,
};

} // namespace aegra::desktop
