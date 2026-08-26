#include "pe_pending_internal.h"

#include <nlohmann/json.hpp>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <utility>

namespace aegra::adapters::windows_pe::detail {
namespace {

using nlohmann::json;

inline constexpr std::string_view kOperationDiskRestore = "pe-disk-restore";
inline constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

[[nodiscard]] base::Error codec_error(const char* message) {
    return {base::ErrorCode::kCorruptData, message};
}

[[nodiscard]] std::string base64_encode(const std::span<const std::byte> input) {
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    std::size_t index = 0;
    while (index + 3 <= input.size()) {
        const auto a = std::to_integer<std::uint32_t>(input[index]);
        const auto b = std::to_integer<std::uint32_t>(input[index + 1]);
        const auto c = std::to_integer<std::uint32_t>(input[index + 2]);
        const std::uint32_t group = (a << 16U) | (b << 8U) | c;
        output.push_back(kBase64Alphabet[(group >> 18U) & 0x3FU]);
        output.push_back(kBase64Alphabet[(group >> 12U) & 0x3FU]);
        output.push_back(kBase64Alphabet[(group >> 6U) & 0x3FU]);
        output.push_back(kBase64Alphabet[group & 0x3FU]);
        index += 3;
    }
    const auto remaining = input.size() - index;
    if (remaining == 1) {
        const auto a = std::to_integer<std::uint32_t>(input[index]);
        output.push_back(kBase64Alphabet[(a >> 2U) & 0x3FU]);
        output.push_back(kBase64Alphabet[(a << 4U) & 0x3FU]);
        output.append("==");
    } else if (remaining == 2) {
        const auto a = std::to_integer<std::uint32_t>(input[index]);
        const auto b = std::to_integer<std::uint32_t>(input[index + 1]);
        output.push_back(kBase64Alphabet[(a >> 2U) & 0x3FU]);
        output.push_back(kBase64Alphabet[((a << 4U) | (b >> 4U)) & 0x3FU]);
        output.push_back(kBase64Alphabet[(b << 2U) & 0x3FU]);
        output.push_back('=');
    }
    return output;
}

[[nodiscard]] int base64_symbol_value(const char symbol) noexcept {
    if (symbol >= 'A' && symbol <= 'Z') {
        return symbol - 'A';
    }
    if (symbol >= 'a' && symbol <= 'z') {
        return symbol - 'a' + 26;
    }
    if (symbol >= '0' && symbol <= '9') {
        return symbol - '0' + 52;
    }
    if (symbol == '+') {
        return 62;
    }
    if (symbol == '/') {
        return 63;
    }
    return -1;
}

[[nodiscard]] base::Result<std::vector<std::byte>> base64_decode(const std::string_view input) {
    if (input.size() % 4 != 0) {
        return base::Result<std::vector<std::byte>>::failure(
            codec_error("base64 field length is invalid"));
    }
    std::vector<std::byte> output;
    output.reserve((input.size() / 4) * 3);
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const bool last_group = index + 4 == input.size();
        std::array<int, 4> values{};
        std::size_t padding = 0;
        for (std::size_t offset = 0; offset < 4; ++offset) {
            const char symbol = input[index + offset];
            if (symbol == '=') {
                if (!last_group || offset < 2) {
                    return base::Result<std::vector<std::byte>>::failure(
                        codec_error("base64 padding is invalid"));
                }
                ++padding;
                values[offset] = 0;
                continue;
            }
            if (padding != 0) {
                return base::Result<std::vector<std::byte>>::failure(
                    codec_error("base64 padding is invalid"));
            }
            values[offset] = base64_symbol_value(symbol);
            if (values[offset] < 0) {
                return base::Result<std::vector<std::byte>>::failure(
                    codec_error("base64 field contains an invalid symbol"));
            }
        }
        const std::uint32_t group =
            (static_cast<std::uint32_t>(values[0]) << 18U) |
            (static_cast<std::uint32_t>(values[1]) << 12U) |
            (static_cast<std::uint32_t>(values[2]) << 6U) | static_cast<std::uint32_t>(values[3]);
        output.push_back(static_cast<std::byte>((group >> 16U) & 0xFFU));
        if (padding < 2) {
            output.push_back(static_cast<std::byte>((group >> 8U) & 0xFFU));
        }
        if (padding < 1) {
            output.push_back(static_cast<std::byte>(group & 0xFFU));
        }
    }
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

[[nodiscard]] base::Result<void>
require_exact_fields(const json& object, const std::initializer_list<const char*> names,
                     const char* what) {
    if (!object.is_object() || object.size() != names.size()) {
        std::string message = "pe document section has an unexpected shape: ";
        message += what;
        return base::Result<void>::failure({base::ErrorCode::kCorruptData, std::move(message)});
    }
    for (const auto* name : names) {
        if (!object.contains(name)) {
            std::string message = "pe document section is missing a field: ";
            message += what;
            return base::Result<void>::failure(
                {base::ErrorCode::kCorruptData, std::move(message)});
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::string> get_string(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_string()) {
        std::string message = "pe document field must be a string: ";
        message += key;
        return base::Result<std::string>::failure(
            {base::ErrorCode::kCorruptData, std::move(message)});
    }
    return base::Result<std::string>::success(found->get<std::string>());
}

[[nodiscard]] base::Result<std::uint64_t> get_uint64(const json& object, const char* key) {
    const auto found = object.find(key);
    const bool non_negative_integer =
        found != object.end() &&
        (found->is_number_unsigned() ||
         (found->is_number_integer() && found->get<std::int64_t>() >= 0));
    if (!non_negative_integer) {
        std::string message = "pe document field must be a non-negative integer: ";
        message += key;
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kCorruptData, std::move(message)});
    }
    return base::Result<std::uint64_t>::success(found->get<std::uint64_t>());
}

[[nodiscard]] base::Result<std::int64_t> get_int64(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) {
        std::string message = "pe document field must be an integer: ";
        message += key;
        return base::Result<std::int64_t>::failure(
            {base::ErrorCode::kCorruptData, std::move(message)});
    }
    return base::Result<std::int64_t>::success(found->get<std::int64_t>());
}

[[nodiscard]] base::Result<bool> get_bool(const json& object, const char* key) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_boolean()) {
        std::string message = "pe document field must be a boolean: ";
        message += key;
        return base::Result<bool>::failure({base::ErrorCode::kCorruptData, std::move(message)});
    }
    return base::Result<bool>::success(found->get<bool>());
}

[[nodiscard]] std::string_view envelope_mode_name(const contracts::PeEnvelopeMode mode) noexcept {
    switch (mode) {
    case contracts::PeEnvelopeMode::kNone:
        return "none";
    case contracts::PeEnvelopeMode::kPrompt:
        return "prompt";
    case contracts::PeEnvelopeMode::kSealed:
        return "sealed";
    }
    return "";
}

[[nodiscard]] std::string_view status_name(const contracts::PeRestoreStatus status) noexcept {
    switch (status) {
    case contracts::PeRestoreStatus::kSuccess:
        return "success";
    case contracts::PeRestoreStatus::kFailed:
        return "failed";
    case contracts::PeRestoreStatus::kCancelled:
        return "cancelled";
    }
    return "";
}

[[nodiscard]] json encode_source(const contracts::PeRestoreSource& source) {
    json chain = json::array();
    for (const auto& layer : source.chain) {
        chain.push_back({{"file_uuid", layer.file_uuid},
                         {"volume_guid", layer.volume_guid},
                         {"relative_path", layer.relative_path},
                         {"online_path", layer.online_path}});
    }
    return {{"repository_uuid", source.repository_uuid},
            {"chain", std::move(chain)},
            {"source_disk_number", source.source_disk_number},
            {"disk_size_bytes", source.disk_size_bytes},
            {"chain_fingerprint", source.chain_fingerprint}};
}

[[nodiscard]] json encode_envelope(const contracts::PeSecretEnvelope& envelope) {
    json encoded = {{"mode", envelope_mode_name(envelope.mode)}};
    if (envelope.mode == contracts::PeEnvelopeMode::kSealed) {
        encoded["ciphertext"] = base64_encode(envelope.ciphertext_with_tag);
        encoded["nonce"] = base64_encode(envelope.nonce);
        encoded["binding_digest"] = envelope.binding_digest_hex;
    }
    return encoded;
}

[[nodiscard]] base::Result<contracts::PeRestoreSource> decode_source(const json& object) {
    using Output = base::Result<contracts::PeRestoreSource>;
    if (auto shape = require_exact_fields(object,
                                          {"repository_uuid", "chain", "source_disk_number",
                                           "disk_size_bytes", "chain_fingerprint"},
                                          "source");
        !shape) {
        return Output::failure(shape.error());
    }
    contracts::PeRestoreSource source;
    auto repository = get_string(object, "repository_uuid");
    auto fingerprint = get_string(object, "chain_fingerprint");
    auto disk_number = get_uint64(object, "source_disk_number");
    auto disk_size = get_uint64(object, "disk_size_bytes");
    if (!repository || !fingerprint || !disk_number || !disk_size) {
        return Output::failure(!repository ? repository.error()
                               : !fingerprint ? fingerprint.error()
                               : !disk_number ? disk_number.error()
                                              : disk_size.error());
    }
    if (disk_number.value() > 0xFFFFFFFFULL) {
        return Output::failure(codec_error("pe document source_disk_number is out of range"));
    }
    source.repository_uuid = std::move(repository).value();
    source.chain_fingerprint = std::move(fingerprint).value();
    source.source_disk_number = static_cast<std::uint32_t>(disk_number.value());
    source.disk_size_bytes = disk_size.value();
    const auto chain = object.find("chain");
    if (chain == object.end() || !chain->is_array()) {
        return Output::failure(codec_error("pe document chain must be an array"));
    }
    for (const auto& entry : *chain) {
        if (auto shape = require_exact_fields(
                entry, {"file_uuid", "volume_guid", "relative_path", "online_path"}, "chain");
            !shape) {
            return Output::failure(shape.error());
        }
        auto file_uuid = get_string(entry, "file_uuid");
        auto volume_guid = get_string(entry, "volume_guid");
        auto relative_path = get_string(entry, "relative_path");
        auto online_path = get_string(entry, "online_path");
        if (!file_uuid || !volume_guid || !relative_path || !online_path) {
            return Output::failure(codec_error("pe document chain layer field is invalid"));
        }
        source.chain.push_back({std::move(file_uuid).value(), std::move(volume_guid).value(),
                                std::move(relative_path).value(), std::move(online_path).value()});
    }
    return Output::success(std::move(source));
}

[[nodiscard]] base::Result<contracts::PeSecretEnvelope> decode_envelope(const json& object) {
    using Output = base::Result<contracts::PeSecretEnvelope>;
    auto mode = get_string(object, "mode");
    if (!mode) {
        return Output::failure(mode.error());
    }
    contracts::PeSecretEnvelope envelope;
    if (mode.value() == "none" || mode.value() == "prompt") {
        if (auto shape = require_exact_fields(object, {"mode"}, "envelope"); !shape) {
            return Output::failure(shape.error());
        }
        envelope.mode = mode.value() == "none" ? contracts::PeEnvelopeMode::kNone
                                               : contracts::PeEnvelopeMode::kPrompt;
        return Output::success(std::move(envelope));
    }
    if (mode.value() != "sealed") {
        return Output::failure(codec_error("pe document envelope mode is unknown"));
    }
    if (auto shape = require_exact_fields(
            object, {"mode", "ciphertext", "nonce", "binding_digest"}, "envelope");
        !shape) {
        return Output::failure(shape.error());
    }
    envelope.mode = contracts::PeEnvelopeMode::kSealed;
    auto ciphertext_text = get_string(object, "ciphertext");
    auto nonce_text = get_string(object, "nonce");
    auto binding = get_string(object, "binding_digest");
    if (!ciphertext_text || !nonce_text || !binding) {
        return Output::failure(codec_error("pe document envelope field is invalid"));
    }
    auto ciphertext = base64_decode(ciphertext_text.value());
    auto nonce = base64_decode(nonce_text.value());
    if (!ciphertext || !nonce) {
        return Output::failure(!ciphertext ? ciphertext.error() : nonce.error());
    }
    envelope.ciphertext_with_tag = std::move(ciphertext).value();
    envelope.nonce = std::move(nonce).value();
    envelope.binding_digest_hex = std::move(binding).value();
    return Output::success(std::move(envelope));
}

[[nodiscard]] base::Result<contracts::PeTargetDiskIdentity> decode_target(const json& object) {
    using Output = base::Result<contracts::PeTargetDiskIdentity>;
    if (auto shape = require_exact_fields(object,
                                          {"serial_number", "size_bytes", "bus_type",
                                           "friendly_name", "partition_style"},
                                          "target");
        !shape) {
        return Output::failure(shape.error());
    }
    auto serial = get_string(object, "serial_number");
    auto size = get_uint64(object, "size_bytes");
    auto bus = get_string(object, "bus_type");
    auto name = get_string(object, "friendly_name");
    auto style = get_string(object, "partition_style");
    if (!serial || !size || !bus || !name || !style) {
        return Output::failure(codec_error("pe document target field is invalid"));
    }
    contracts::PeTargetDiskIdentity target;
    target.serial_number = std::move(serial).value();
    target.size_bytes = size.value();
    target.bus_type = std::move(bus).value();
    target.friendly_name = std::move(name).value();
    target.partition_style = std::move(style).value();
    return Output::success(std::move(target));
}

[[nodiscard]] base::Result<contracts::PeRestoreStatus> decode_status(const std::string& value) {
    if (value == "success") {
        return base::Result<contracts::PeRestoreStatus>::success(
            contracts::PeRestoreStatus::kSuccess);
    }
    if (value == "failed") {
        return base::Result<contracts::PeRestoreStatus>::success(
            contracts::PeRestoreStatus::kFailed);
    }
    if (value == "cancelled") {
        return base::Result<contracts::PeRestoreStatus>::success(
            contracts::PeRestoreStatus::kCancelled);
    }
    return base::Result<contracts::PeRestoreStatus>::failure(
        codec_error("pe result status is unknown"));
}

} // namespace

base::Result<std::string> encode_pe_pending_job(const contracts::PePendingJobV1& job) {
    if (auto valid = contracts::validate_pe_pending_job(job); !valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    const json document = {{"schema_version", job.schema_version},
                           {"job_uuid", job.job_uuid},
                           {"created_utc_ms", job.created_utc_ms},
                           {"product_version", job.product_version},
                           {"operation", kOperationDiskRestore},
                           {"source", encode_source(job.source)},
                           {"envelope", encode_envelope(job.envelope)},
                           {"target",
                            {{"serial_number", job.target.serial_number},
                             {"size_bytes", job.target.size_bytes},
                             {"bus_type", job.target.bus_type},
                             {"friendly_name", job.target.friendly_name},
                             {"partition_style", job.target.partition_style}}},
                           {"options",
                            {{"preserve_disk_signature", job.options.preserve_disk_signature},
                             {"auto_expand_last_partition",
                              job.options.auto_expand_last_partition}}},
                           {"ui",
                            {{"locale", job.ui.locale},
                             {"auto_start_seconds", job.ui.auto_start_seconds}}}};
    return base::Result<std::string>::success(document.dump(2));
}

base::Result<contracts::PePendingJobV1> decode_pe_pending_job(const std::string_view json_text) {
    using Output = base::Result<contracts::PePendingJobV1>;
    const auto document = json::parse(json_text, nullptr, false);
    if (document.is_discarded()) {
        return Output::failure(codec_error("pe pending job document is not valid json"));
    }
    if (auto shape = require_exact_fields(document,
                                          {"schema_version", "job_uuid", "created_utc_ms",
                                           "product_version", "operation", "source", "envelope",
                                           "target", "options", "ui"},
                                          "job");
        !shape) {
        return Output::failure(shape.error());
    }
    auto schema = get_uint64(document, "schema_version");
    if (!schema) {
        return Output::failure(schema.error());
    }
    if (schema.value() != contracts::kPePendingJobSchemaVersion) {
        return Output::failure({base::ErrorCode::kUnsupportedVersion,
                                "pe pending job schema version is unsupported"});
    }
    auto operation = get_string(document, "operation");
    if (!operation || operation.value() != kOperationDiskRestore) {
        return Output::failure(codec_error("pe pending job operation is unsupported"));
    }
    contracts::PePendingJobV1 job;
    auto job_uuid = get_string(document, "job_uuid");
    auto created = get_int64(document, "created_utc_ms");
    auto product = get_string(document, "product_version");
    if (!job_uuid || !created || !product) {
        return Output::failure(!job_uuid ? job_uuid.error()
                               : !created ? created.error()
                                          : product.error());
    }
    job.job_uuid = std::move(job_uuid).value();
    job.created_utc_ms = created.value();
    job.product_version = std::move(product).value();
    auto source = decode_source(document.at("source"));
    auto envelope = decode_envelope(document.at("envelope"));
    auto target = decode_target(document.at("target"));
    if (!source || !envelope || !target) {
        return Output::failure(!source ? source.error()
                               : !envelope ? envelope.error()
                                           : target.error());
    }
    job.source = std::move(source).value();
    job.envelope = std::move(envelope).value();
    job.target = std::move(target).value();
    const auto& options = document.at("options");
    const auto& ui = document.at("ui");
    if (auto shape = require_exact_fields(
            options, {"preserve_disk_signature", "auto_expand_last_partition"}, "options");
        !shape) {
        return Output::failure(shape.error());
    }
    if (auto shape = require_exact_fields(ui, {"locale", "auto_start_seconds"}, "ui"); !shape) {
        return Output::failure(shape.error());
    }
    auto preserve = get_bool(options, "preserve_disk_signature");
    auto expand = get_bool(options, "auto_expand_last_partition");
    auto locale = get_string(ui, "locale");
    auto auto_start = get_uint64(ui, "auto_start_seconds");
    if (!preserve || !expand || !locale || !auto_start ||
        auto_start.value() > contracts::kMaximumPeAutoStartSeconds) {
        return Output::failure(codec_error("pe pending job options or ui field is invalid"));
    }
    job.options.preserve_disk_signature = preserve.value();
    job.options.auto_expand_last_partition = expand.value();
    job.ui.locale = std::move(locale).value();
    job.ui.auto_start_seconds = static_cast<std::uint32_t>(auto_start.value());
    if (auto valid = contracts::validate_pe_pending_job(job); !valid) {
        return Output::failure(valid.error());
    }
    return Output::success(std::move(job));
}

base::Result<std::string> encode_pe_restore_result(const contracts::PeRestoreResultV1& result) {
    if (auto valid = contracts::validate_pe_restore_result(result); !valid) {
        return base::Result<std::string>::failure(valid.error());
    }
    const json document = {{"schema_version", result.schema_version},
                           {"job_uuid", result.job_uuid},
                           {"finished_utc_ms", result.finished_utc_ms},
                           {"status", status_name(result.status)},
                           {"error_code", result.error_code},
                           {"error_message", result.error_message},
                           {"log_relative_path", result.log_relative_path}};
    return base::Result<std::string>::success(document.dump(2));
}

base::Result<contracts::PeRestoreResultV1>
decode_pe_restore_result(const std::string_view json_text) {
    using Output = base::Result<contracts::PeRestoreResultV1>;
    const auto document = json::parse(json_text, nullptr, false);
    if (document.is_discarded()) {
        return Output::failure(codec_error("pe restore result document is not valid json"));
    }
    if (auto shape = require_exact_fields(document,
                                          {"schema_version", "job_uuid", "finished_utc_ms",
                                           "status", "error_code", "error_message",
                                           "log_relative_path"},
                                          "result");
        !shape) {
        return Output::failure(shape.error());
    }
    auto schema = get_uint64(document, "schema_version");
    if (!schema) {
        return Output::failure(schema.error());
    }
    if (schema.value() != contracts::kPeRestoreResultSchemaVersion) {
        return Output::failure({base::ErrorCode::kUnsupportedVersion,
                                "pe restore result schema version is unsupported"});
    }
    contracts::PeRestoreResultV1 result;
    auto job_uuid = get_string(document, "job_uuid");
    auto finished = get_int64(document, "finished_utc_ms");
    auto status_text = get_string(document, "status");
    auto error_code = get_string(document, "error_code");
    auto error_message = get_string(document, "error_message");
    auto log_path = get_string(document, "log_relative_path");
    if (!job_uuid || !finished || !status_text || !error_code || !error_message || !log_path) {
        return Output::failure(codec_error("pe restore result field is invalid"));
    }
    auto status = decode_status(status_text.value());
    if (!status) {
        return Output::failure(status.error());
    }
    result.job_uuid = std::move(job_uuid).value();
    result.finished_utc_ms = finished.value();
    result.status = status.value();
    result.error_code = std::move(error_code).value();
    result.error_message = std::move(error_message).value();
    result.log_relative_path = std::move(log_path).value();
    if (auto valid = contracts::validate_pe_restore_result(result); !valid) {
        return Output::failure(valid.error());
    }
    return Output::success(std::move(result));
}

} // namespace aegra::adapters::windows_pe::detail
