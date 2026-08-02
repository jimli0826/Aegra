#pragma once

#include "aegra/apps/worker/windows_personal_backup_task.h"
#include "aegra/apps/worker/worker_host.h"
#include "aegra/base/cancellation.h"
#include "aegra/ports/message_channel.h"

namespace aegra::apps::worker {

// Receives one encoded Job, listens for one correlated Cancel command, streams Progress events,
// and sends exactly one final Result event over a full-duplex message channel.
[[nodiscard]] WorkerExitCode run_windows_personal_backup_worker_session(
    ports::IMessageChannel& channel, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context, const base::CancellationToken& cancellation);

} // namespace aegra::apps::worker
