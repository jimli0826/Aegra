#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace aegra::contracts {

inline constexpr std::uint32_t kPePendingJobSchemaVersion = 1;
inline constexpr std::uint32_t kPeRestoreResultSchemaVersion = 1;

/// Wire sizes of the sealed secret envelope (XChaCha20-Poly1305; ADR-0026).
/// The crypto adapter static-asserts its algorithm constants against these values.
inline constexpr std::size_t kPeEnvelopeKeySize = 32;
inline constexpr std::size_t kPeEnvelopeNonceSize = 24;
inline constexpr std::size_t kPeEnvelopeTagSize = 16;
inline constexpr std::size_t kPeBindingDigestHexSize = 64;

inline constexpr std::size_t kMaximumPeChainLayers = 64;
inline constexpr std::uint32_t kMaximumPeAutoStartSeconds = 300;

enum class PeEnvelopeMode : std::uint8_t {
    /// Unencrypted archive: no secret material anywhere.
    kNone = 1,
    /// No secret material on disk; the PE executor prompts interactively.
    kPrompt = 2,
    /// Password sealed with a detached short-lived job key (ADR-0026 B).
    kSealed = 3,
};

enum class PeRestoreStatus : std::uint8_t {
    kSuccess = 1,
    kFailed = 2,
    kCancelled = 3,
};

/// One archive layer of the base-first restore chain. Located by volume identity:
/// drive letters are not stable inside WinPE.
struct PeChainLayer final {
    std::string file_uuid;
    /// Canonical volume GUID path (`\\?\Volume{...}\`).
    std::string volume_guid;
    /// Path relative to the volume root; must not contain `..` or a drive prefix.
    std::string relative_path;
    /// Online drive-letter path. Logs and messages only; never used to open files in PE.
    std::string online_path;
};

struct PeRestoreSource final {
    std::string repository_uuid;
    /// Base-first, tip last. Resolved and frozen online; PE performs no chain discovery.
    std::vector<PeChainLayer> chain;
    std::uint32_t source_disk_number{0};
    std::uint64_t disk_size_bytes{0};
    /// Same fingerprint algorithm as the online disk-restore path; part of the AEAD binding.
    std::string chain_fingerprint;
};

struct PeSecretEnvelope final {
    PeEnvelopeMode mode{PeEnvelopeMode::kNone};
    /// kSealed only: AEAD ciphertext with the 16-byte tag appended.
    std::vector<std::byte> ciphertext_with_tag;
    /// kSealed only: 24-byte AEAD nonce.
    std::vector<std::byte> nonce;
    /// kSealed only: informational lowercase-hex SHA-256 of the binding preimage.
    /// Consumers must recompute the binding; this field only enables an early,
    /// user-readable tamper diagnosis before AEAD verification fails.
    std::string binding_digest_hex;
};

struct PeTargetDiskIdentity final {
    /// Primary re-match key inside PE. Jobs without a serial number are invalid.
    std::string serial_number;
    std::uint64_t size_bytes{0};
    std::string bus_type;
    std::string friendly_name;
    /// "mbr" | "gpt" (same convention as service_control inventory).
    std::string partition_style;
};

struct PeRestoreOptions final {
    bool preserve_disk_signature{true};
    bool auto_expand_last_partition{false};
};

struct PeRestoreUi final {
    std::string locale;
    std::uint32_t auto_start_seconds{10};
};

/// Cross-reboot pending job consumed exactly once by the WinPE executor.
struct PePendingJobV1 final {
    std::uint32_t schema_version{kPePendingJobSchemaVersion};
    std::string job_uuid;
    std::int64_t created_utc_ms{0};
    std::string product_version;
    PeRestoreSource source;
    PeSecretEnvelope envelope;
    PeTargetDiskIdentity target;
    PeRestoreOptions options;
    PeRestoreUi ui;
};

/// Result written by the PE executor and consumed by the service on next boot.
struct PeRestoreResultV1 final {
    std::uint32_t schema_version{kPeRestoreResultSchemaVersion};
    std::string job_uuid;
    std::int64_t finished_utc_ms{0};
    PeRestoreStatus status{PeRestoreStatus::kFailed};
    /// Stable `pe_restore.*` message code; required unless status is kSuccess.
    std::string error_code;
    std::string error_message;
    /// Relative to the data directory (forward or back slashes, no `..`).
    std::string log_relative_path;
};

[[nodiscard]] base::Result<void> validate_pe_pending_job(const PePendingJobV1& job);
[[nodiscard]] base::Result<void> validate_pe_restore_result(const PeRestoreResultV1& result);

/// Canonical AEAD binding preimage (ADR-0026 B):
/// `<schema_version>|<job_uuid>|<chain_fingerprint>|<target serial>|<target size>`.
/// Tampering with any bound field makes the sealed envelope fail to open.
[[nodiscard]] std::string pe_pending_job_binding(const PePendingJobV1& job);

} // namespace aegra::contracts
