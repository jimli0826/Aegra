#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/contracts/pe_restore.h"

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

namespace aegra::ports {

/// Cross-reboot pending store for the WinPE offline restore hand-off (ADR-0026).
///
/// Implementations must:
/// - persist the job atomically (temp file + rename) and validate on both directions;
/// - keep the sealed job key in a separate file with a restricted ACL
///   (SYSTEM / Administrators only), never inside the job document;
/// - never log key material or envelope ciphertext;
/// - tolerate repeated clear/consume calls (idempotent cleanup paths).
class IPePendingJobStore {
  public:
    IPePendingJobStore() = default;
    virtual ~IPePendingJobStore() = default;
    IPePendingJobStore(const IPePendingJobStore&) = delete;
    IPePendingJobStore& operator=(const IPePendingJobStore&) = delete;
    IPePendingJobStore(IPePendingJobStore&&) = delete;
    IPePendingJobStore& operator=(IPePendingJobStore&&) = delete;

    /// Create-only single occupancy: fails with kConflict while a pending job exists.
    /// `job_key` must be exactly contracts::kPeEnvelopeKeySize bytes when the job
    /// envelope mode is kSealed and empty otherwise.
    [[nodiscard]] virtual base::Result<void>
    write_pending(const contracts::PePendingJobV1& job, std::span<const std::byte> job_key,
                  base::CancellationToken cancellation) = 0;

    /// kNotFound when no pending job exists. The returned job is already validated.
    [[nodiscard]] virtual base::Result<contracts::PePendingJobV1>
    read_pending(base::CancellationToken cancellation) = 0;

    /// Sealed job key bytes; kNotFound when absent. The caller must zeroize the
    /// returned buffer once the envelope has been opened.
    [[nodiscard]] virtual base::Result<std::vector<std::byte>>
    read_job_key(base::CancellationToken cancellation) = 0;

    /// Overwrites the key file content and deletes it (use-once semantics).
    /// Succeeds when the key file is already gone.
    [[nodiscard]] virtual base::Result<void> consume_job_key() = 0;

    /// Deletes the pending job and consumes the key (cancel / rollback path).
    /// Succeeds when nothing is pending.
    [[nodiscard]] virtual base::Result<void> clear_pending() = 0;

    /// Atomically writes (or replaces) the restore result document.
    [[nodiscard]] virtual base::Result<void>
    write_result(const contracts::PeRestoreResultV1& result,
                 base::CancellationToken cancellation) = 0;

    /// std::nullopt when no result document exists.
    [[nodiscard]] virtual base::Result<std::optional<contracts::PeRestoreResultV1>>
    read_result(base::CancellationToken cancellation) = 0;

    /// Deletes the result document. Succeeds when it is already gone.
    [[nodiscard]] virtual base::Result<void> clear_result() = 0;
};

} // namespace aegra::ports
