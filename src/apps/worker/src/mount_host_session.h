#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/message_channel.h"

namespace aegra::apps::worker {

// Runs one mount session over an already-connected Service pipe.
// Returns success after unmount/cleanup; failure only for hard host errors.
[[nodiscard]] base::Result<void>
run_mount_host_session(ports::IMessageChannel& channel, base::CancellationToken cancellation);

} // namespace aegra::apps::worker
