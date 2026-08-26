#include "aegra/contracts/pe_restore.h"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace aegra::contracts {
namespace {

[[nodiscard]] base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

[[nodiscard]] bool valid_text(const std::string& value, const std::size_t maximum_size) {
    return !value.empty() && value.size() <= maximum_size;
}

[[nodiscard]] bool valid_optional_text(const std::string& value, const std::size_t maximum_size) {
    return value.size() <= maximum_size;
}

[[nodiscard]] bool is_lowercase_hex(const std::string_view value) {
    return std::ranges::all_of(value, [](const char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

/// Canonical volume GUID path: `\\?\Volume{...}\` with a trailing backslash.
[[nodiscard]] bool valid_volume_guid_path(const std::string_view value) {
    constexpr std::string_view prefix = R"(\\?\Volume{)";
    return value.size() <= 128 && value.starts_with(prefix) && value.ends_with(R"(}\)");
}

/// Volume-relative location: no traversal, no drive prefix, no absolute root.
[[nodiscard]] bool valid_relative_path(const std::string_view value) {
    if (value.empty() || value.size() > 1024) {
        return false;
    }
    if (value.front() == '\\' || value.front() == '/' ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    return value.find("..") == std::string_view::npos;
}

[[nodiscard]] base::Result<void> validate_chain_layer(const PeChainLayer& layer) {
    if (!valid_text(layer.file_uuid, 64)) {
        return invalid("pe chain layer file_uuid is invalid");
    }
    if (!valid_volume_guid_path(layer.volume_guid)) {
        return invalid("pe chain layer volume_guid must be a canonical volume GUID path");
    }
    if (!valid_relative_path(layer.relative_path)) {
        return invalid("pe chain layer relative_path is invalid");
    }
    if (!valid_optional_text(layer.online_path, 1024)) {
        return invalid("pe chain layer online_path is too long");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_source(const PeRestoreSource& source) {
    if (!valid_text(source.repository_uuid, 64)) {
        return invalid("pe job repository_uuid is invalid");
    }
    if (source.chain.empty() || source.chain.size() > kMaximumPeChainLayers) {
        return invalid("pe job chain must contain 1..64 base-first layers");
    }
    for (const auto& layer : source.chain) {
        if (auto valid = validate_chain_layer(layer); !valid) {
            return valid;
        }
    }
    if (source.disk_size_bytes == 0) {
        return invalid("pe job source disk size must be positive");
    }
    // The online disk-restore fingerprint ("diskc|...") embeds every layer's archive
    // key, so deep chains legitimately exceed a short cap.
    if (!valid_text(source.chain_fingerprint, 4096)) {
        return invalid("pe job chain_fingerprint is invalid");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_sealed_envelope(const PeSecretEnvelope& envelope) {
    if (envelope.nonce.size() != kPeEnvelopeNonceSize) {
        return invalid("pe sealed envelope nonce size is invalid");
    }
    if (envelope.ciphertext_with_tag.size() <= kPeEnvelopeTagSize) {
        return invalid("pe sealed envelope ciphertext is too short");
    }
    if (envelope.ciphertext_with_tag.size() > 4096) {
        return invalid("pe sealed envelope ciphertext is too long");
    }
    if (envelope.binding_digest_hex.size() != kPeBindingDigestHexSize ||
        !is_lowercase_hex(envelope.binding_digest_hex)) {
        return invalid("pe sealed envelope binding digest must be lowercase hex sha-256");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_envelope(const PeSecretEnvelope& envelope) {
    switch (envelope.mode) {
    case PeEnvelopeMode::kSealed:
        return validate_sealed_envelope(envelope);
    case PeEnvelopeMode::kNone:
    case PeEnvelopeMode::kPrompt:
        if (!envelope.ciphertext_with_tag.empty() || !envelope.nonce.empty() ||
            !envelope.binding_digest_hex.empty()) {
            return invalid("pe envelope carries secret material without sealed mode");
        }
        return base::Result<void>::success();
    }
    return invalid("pe envelope mode is invalid");
}

[[nodiscard]] base::Result<void> validate_target(const PeTargetDiskIdentity& target) {
    if (!valid_text(target.serial_number, 128)) {
        return invalid("pe job target serial_number is required");
    }
    if (target.size_bytes == 0) {
        return invalid("pe job target size must be positive");
    }
    if (!valid_optional_text(target.bus_type, 32) ||
        !valid_optional_text(target.friendly_name, 256)) {
        return invalid("pe job target description is too long");
    }
    if (target.partition_style != "mbr" && target.partition_style != "gpt") {
        return invalid("pe job target partition_style must be mbr or gpt");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> validate_ui(const PeRestoreUi& ui) {
    if (!valid_text(ui.locale, 16)) {
        return invalid("pe job ui locale is required");
    }
    if (ui.auto_start_seconds > kMaximumPeAutoStartSeconds) {
        return invalid("pe job auto_start_seconds is out of range");
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<void> validate_pe_pending_job(const PePendingJobV1& job) {
    if (job.schema_version != kPePendingJobSchemaVersion) {
        return base::Result<void>::failure(
            {base::ErrorCode::kUnsupportedVersion, "pe pending job schema version is unsupported"});
    }
    if (!valid_text(job.job_uuid, 64)) {
        return invalid("pe job job_uuid is invalid");
    }
    if (job.created_utc_ms < 0) {
        return invalid("pe job created_utc_ms must not be negative");
    }
    if (!valid_text(job.product_version, 32)) {
        return invalid("pe job product_version is invalid");
    }
    if (auto valid = validate_source(job.source); !valid) {
        return valid;
    }
    if (auto valid = validate_envelope(job.envelope); !valid) {
        return valid;
    }
    if (auto valid = validate_target(job.target); !valid) {
        return valid;
    }
    return validate_ui(job.ui);
}

base::Result<void> validate_pe_restore_result(const PeRestoreResultV1& result) {
    if (result.schema_version != kPeRestoreResultSchemaVersion) {
        return base::Result<void>::failure({base::ErrorCode::kUnsupportedVersion,
                                            "pe restore result schema version is unsupported"});
    }
    if (!valid_text(result.job_uuid, 64)) {
        return invalid("pe result job_uuid is invalid");
    }
    if (result.finished_utc_ms < 0) {
        return invalid("pe result finished_utc_ms must not be negative");
    }
    if (result.status != PeRestoreStatus::kSuccess && result.status != PeRestoreStatus::kFailed &&
        result.status != PeRestoreStatus::kCancelled) {
        return invalid("pe result status is invalid");
    }
    if (result.status != PeRestoreStatus::kSuccess && !valid_text(result.error_code, 128)) {
        return invalid("pe result error_code is required unless status is success");
    }
    if (result.status == PeRestoreStatus::kSuccess && !result.error_code.empty()) {
        return invalid("pe result error_code must be empty on success");
    }
    if (!valid_optional_text(result.error_message, 4096)) {
        return invalid("pe result error_message is too long");
    }
    if (!result.log_relative_path.empty() && !valid_relative_path(result.log_relative_path)) {
        return invalid("pe result log_relative_path is invalid");
    }
    return base::Result<void>::success();
}

std::string pe_pending_job_binding(const PePendingJobV1& job) {
    std::string binding = std::to_string(job.schema_version);
    binding += '|';
    binding += job.job_uuid;
    binding += '|';
    binding += job.source.chain_fingerprint;
    binding += '|';
    binding += job.target.serial_number;
    binding += '|';
    binding += std::to_string(job.target.size_bytes);
    return binding;
}

} // namespace aegra::contracts
