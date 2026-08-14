#include "aegra/apps/worker/personal_file_archive_restore_task.h"

#include "worker_task_log.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_disk/windows_disk.h"
#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/pipeline/file_set_restore_pipeline.h"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <filesystem>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::worker {
namespace detail {
namespace {

base::Result<void> invalid(const char* message) {
    return base::Result<void>::failure({base::ErrorCode::kInvalidArgument, message});
}

base::Result<void> validate_task(const contracts::JobRequest& job,
                                 const WindowsPersonalBackupTaskOptions& options) {
    auto valid_job = contracts::validate_job_request(job);
    if (!valid_job) {
        return valid_job;
    }
    if (job.content_kind != contracts::ContentKind::kFileSet ||
        job.operation != contracts::JobOperation::kRestore || !job.file_restore_target ||
        job.source_refs.empty() || job.source_refs.size() > contracts::kMaximumFileChainDepth ||
        job.credential_refs.size() != job.source_refs.size() || !job.target_ref.empty()) {
        return invalid("file_set restore requires content_kind=file_set, base-first source chain, "
                       "matching credentials, file_restore_target");
    }
    if (options.memory_budget_bytes == 0) {
        return invalid("file_set restore memory budget is invalid");
    }
    return base::Result<void>::success();
}

std::filesystem::path path_from_utf8(const std::string& value) {
    std::u8string encoded;
    encoded.reserve(value.size());
    for (const char item : value) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    return std::filesystem::path(encoded);
}

const char* message_code_for(const base::ErrorCode code) noexcept {
    switch (code) {
    case base::ErrorCode::kCancelled:
        return "file_restore.cancelled";
    case base::ErrorCode::kUnauthorized:
        return "file_restore.credential_unavailable";
    case base::ErrorCode::kInsufficientSpace:
        return "file_restore.target_full";
    case base::ErrorCode::kNotFound:
    case base::ErrorCode::kIoFailure:
        return "file_restore.io_failed";
    case base::ErrorCode::kCorruptData:
        return "file_restore.failed_before_write";
    case base::ErrorCode::kConflict:
        return "file_restore.target_collision";
    case base::ErrorCode::kInvalidArgument:
        return "file_restore.invalid_request";
    default:
        return "file_restore.failed";
    }
}

[[nodiscard]] bool is_product_message_code(const std::string& message) noexcept {
    return message.starts_with("file_restore.") || message.starts_with("file_source.") ||
           message.starts_with("format.");
}

[[nodiscard]] std::string_view restore_hint_for(const base::ErrorCode code,
                                                const std::string_view message) noexcept {
    if (message.find("password") != std::string_view::npos ||
        code == base::ErrorCode::kUnauthorized) {
        return "Re-enter the archive password and retry";
    }
    if (code == base::ErrorCode::kInsufficientSpace) {
        return "Free space on the target volume or choose another directory";
    }
    if (code == base::ErrorCode::kCorruptData) {
        return "Archive authentication failed; re-backup or pick another recovery point";
    }
    if (code == base::ErrorCode::kCancelled) {
        return "Job was cancelled or deadline expired";
    }
    return "Inspect error_message and retry after fixing the reported condition";
}

contracts::TaskResult failed_result(const contracts::JobRequest& job, const base::ErrorCode code,
                                    const base::Error* detail = nullptr) {
    const auto outcome = code == base::ErrorCode::kCancelled ? contracts::TaskOutcome::kCancelled
                                                             : contracts::TaskOutcome::kFailed;
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    result.outcome = outcome;
    result.error_code = code;
    if (detail != nullptr && is_product_message_code(detail->message)) {
        result.message_code = detail->message;
    } else {
        result.message_code = message_code_for(code);
    }
    return result;
}

contracts::TaskResult completed_result(const contracts::JobRequest& job,
                                       const pipeline::FileSetRestoreSummary& summary) {
    contracts::TaskResult result;
    result.job_id = job.job_id;
    result.trace_id = job.trace_id;
    const bool partial = summary.stats.entries_failed > 0;
    result.outcome = partial ? contracts::TaskOutcome::kSucceededWithWarning
                             : contracts::TaskOutcome::kSucceeded;
    result.error_code = base::ErrorCode::kNone;
    result.logical_bytes = summary.stats.bytes_restored;
    result.stored_bytes = summary.stats.bytes_restored;
    result.entry_count = summary.stats.entries_restored;
    result.message_code = partial ? "file_restore.partial" : "file_restore.completed";
    if (partial) {
        // validate_task_result(SucceededWithWarning) requires non-empty warning_codes.
        result.warning_codes = summary.stats.stable_error_codes;
        if (result.warning_codes.empty()) {
            result.warning_codes.push_back("file_restore.partial");
        }
        result.partial_restore = summary.stats;
    }
    return result;
}

base::Result<contracts::TaskResult> validated_result(contracts::TaskResult result) {
    auto validation = contracts::validate_task_result(result);
    if (!validation) {
        return base::Result<contracts::TaskResult>::failure(validation.error());
    }
    return base::Result<contracts::TaskResult>::success(std::move(result));
}

class EmptyPasswordSecret final : public ports::IResolvedSecret {
  public:
    [[nodiscard]] std::string_view view() const noexcept override {
        return {};
    }
};

[[nodiscard]] base::Result<std::unique_ptr<ports::IResolvedSecret>>
resolve_one_secret(const contracts::SecretRef& credential, ports::ICredentialResolver& credentials,
                   const base::CancellationToken& cancellation) {
    if (credential.value.empty()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::success(
            std::make_unique<EmptyPasswordSecret>());
    }
    auto resolved = credentials.resolve(credential, cancellation);
    if (!resolved || resolved.value() == nullptr || resolved.value()->view().empty()) {
        const auto code = !resolved && resolved.error().code == base::ErrorCode::kCancelled
                              ? base::ErrorCode::kCancelled
                              : base::ErrorCode::kUnauthorized;
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            {code, !resolved ? resolved.error().message : "archive credential is unavailable"});
    }
    return resolved;
}

[[nodiscard]] base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>
resolve_restore_secrets(const contracts::JobRequest& job, ports::ICredentialResolver& credentials,
                        const base::CancellationToken& cancellation) {
    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    secrets.reserve(job.credential_refs.size());
    for (const auto& credential : job.credential_refs) {
        auto resolved = resolve_one_secret(credential, credentials, cancellation);
        if (!resolved) {
            return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::failure(
                resolved.error());
        }
        secrets.push_back(std::move(resolved).value());
    }
    return base::Result<std::vector<std::unique_ptr<ports::IResolvedSecret>>>::success(
        std::move(secrets));
}

[[nodiscard]] int path_hex_value(const char character) noexcept {
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

[[nodiscard]] base::Result<std::vector<contracts::EncodedName>>
decode_relative_path_blob(const std::string_view encoded) {
    if (!encoded.starts_with("v1")) {
        return base::Result<std::vector<contracts::EncodedName>>::failure(
            {base::ErrorCode::kInvalidArgument, "target path blob version is unsupported"});
    }
    std::vector<contracts::EncodedName> components;
    std::size_t index = 2;
    while (index < encoded.size()) {
        if (encoded[index] != '|') {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                {base::ErrorCode::kInvalidArgument, "target path blob is corrupt"});
        }
        ++index;
        const std::size_t colon = encoded.find(':', index);
        if (colon == std::string_view::npos || colon == index) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                {base::ErrorCode::kInvalidArgument, "target path blob is corrupt"});
        }
        unsigned encoding = 0;
        for (std::size_t cursor = index; cursor < colon; ++cursor) {
            if (encoded[cursor] < '0' || encoded[cursor] > '9') {
                return base::Result<std::vector<contracts::EncodedName>>::failure(
                    {base::ErrorCode::kInvalidArgument, "target path blob is corrupt"});
            }
            encoding = encoding * 10U + static_cast<unsigned>(encoded[cursor] - '0');
        }
        index = colon + 1;
        std::size_t next = encoded.find('|', index);
        if (next == std::string_view::npos) {
            next = encoded.size();
        }
        const auto hex = encoded.substr(index, next - index);
        if ((hex.size() % 2U) != 0U) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                {base::ErrorCode::kInvalidArgument, "target path blob is corrupt"});
        }
        contracts::EncodedName name;
        name.encoding = static_cast<contracts::NameEncoding>(encoding);
        name.bytes.reserve(hex.size() / 2U);
        for (std::size_t byte_index = 0; byte_index < hex.size(); byte_index += 2) {
            const int high = path_hex_value(hex[byte_index]);
            const int low = path_hex_value(hex[byte_index + 1]);
            if (high < 0 || low < 0) {
                return base::Result<std::vector<contracts::EncodedName>>::failure(
                    {base::ErrorCode::kInvalidArgument, "target path blob is corrupt"});
            }
            name.bytes.push_back(static_cast<std::byte>((high << 4) | low));
        }
        components.push_back(std::move(name));
        index = next;
    }
    return base::Result<std::vector<contracts::EncodedName>>::success(std::move(components));
}

struct ParsedTargetRoot final {
    std::string volume_identity;
    std::vector<contracts::EncodedName> relative_components;
};

[[nodiscard]] base::Result<ParsedTargetRoot>
parse_target_root_identity(const std::string_view identity) {
    if (!identity.starts_with("f1|")) {
        return base::Result<ParsedTargetRoot>::failure(
            {base::ErrorCode::kInvalidArgument, "file restore target_root_identity is invalid"});
    }
    const auto rest = identity.substr(3);
    const auto bar = rest.find('|');
    if (bar == std::string_view::npos || bar == 0) {
        return base::Result<ParsedTargetRoot>::failure(
            {base::ErrorCode::kInvalidArgument, "file restore target_root_identity is invalid"});
    }
    ParsedTargetRoot parsed;
    parsed.volume_identity = std::string(rest.substr(0, bar));
    auto components = decode_relative_path_blob(rest.substr(bar + 1));
    if (!components) {
        return base::Result<ParsedTargetRoot>::failure(components.error());
    }
    parsed.relative_components = std::move(components).value();
    return base::Result<ParsedTargetRoot>::success(std::move(parsed));
}

[[nodiscard]] std::wstring lower_native(const std::filesystem::path& path) {
    auto value = path.native();
    std::ranges::transform(value, value.begin(),
                           [](const wchar_t character) { return std::towlower(character); });
    return value;
}

[[nodiscard]] base::Result<std::vector<std::uint16_t>>
resolve_target_root_utf16(const ParsedTargetRoot& target) {
    std::filesystem::path guid_path = path_from_utf8(target.volume_identity);
    if (!adapters::windows_disk::WindowsBlockSource::is_canonical_volume_guid_path(guid_path)) {
        return base::Result<std::vector<std::uint16_t>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.volume_identity_mismatch"});
    }
    auto inventory = adapters::windows_disk::WindowsVolumeEnumerator::enumerate();
    if (!inventory) {
        return base::Result<std::vector<std::uint16_t>>::failure(inventory.error());
    }
    const auto expected = lower_native(guid_path);
    const auto selected = std::ranges::find_if(inventory.value(), [&expected](const auto& volume) {
        return lower_native(volume.volume_guid_path) == expected;
    });
    if (selected == inventory.value().end()) {
        return base::Result<std::vector<std::uint16_t>>::failure(
            {base::ErrorCode::kNotFound, "file restore target volume was not found"});
    }
    if (selected->is_read_only) {
        return base::Result<std::vector<std::uint16_t>>::failure(
            {base::ErrorCode::kConflict, "file restore target volume is read-only"});
    }
    std::wstring path = selected->volume_guid_path.native();
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    for (const auto& component : target.relative_components) {
        if (component.bytes.empty() || (component.bytes.size() % 2U) != 0U) {
            return base::Result<std::vector<std::uint16_t>>::failure(
                {base::ErrorCode::kInvalidArgument, "file restore target path is invalid"});
        }
        std::wstring piece(component.bytes.size() / 2U, L'\0');
        std::memcpy(piece.data(), component.bytes.data(), component.bytes.size());
        path.push_back(L'\\');
        path.append(piece);
    }
    return base::Result<std::vector<std::uint16_t>>::success(
        std::vector<std::uint16_t>(path.begin(), path.end()));
}

[[nodiscard]] base::Result<std::vector<std::uint64_t>>
parse_entry_ids(const std::vector<std::string>& texts) {
    std::vector<std::uint64_t> ids;
    ids.reserve(texts.size());
    for (const auto& text : texts) {
        std::uint64_t value = 0;
        if (text.empty() || text.size() > 20) {
            return base::Result<std::vector<std::uint64_t>>::failure(
                {base::ErrorCode::kInvalidArgument, "file restore entry_ids are invalid"});
        }
        const auto* begin = text.data();
        const auto* end = begin + text.size();
        if (std::from_chars(begin, end, value).ec != std::errc{} || value == 0) {
            return base::Result<std::vector<std::uint64_t>>::failure(
                {base::ErrorCode::kInvalidArgument, "file restore entry_ids are invalid"});
        }
        ids.push_back(value);
    }
    return base::Result<std::vector<std::uint64_t>>::success(std::move(ids));
}

[[nodiscard]] base::Result<contracts::TaskResult>
run_file_set_restore(const contracts::JobRequest& job,
                     const WindowsPersonalBackupTaskOptions& options,
                     const WindowsPersonalBackupTaskContext& context,
                     const base::CancellationToken& cancellation) {
    auto task_log = WorkerTaskLog::open("restore", job.job_id);
    const auto started = std::chrono::steady_clock::now();
    if (task_log) {
        task_log->section("Job");
        task_log->field("job_id", job.job_id);
        task_log->field("trace_id", job.trace_id);
        task_log->field("operation", "restore");
        task_log->section("Request");
        task_log->field("content_kind", "file_set");
        task_log->field_u64("entry_ids", job.file_restore_target->entry_ids.size());
    }
    if (cancellation.stop_requested() ||
        (job.deadline_utc_ms > 0 && context.clock.now_utc_ms() >= job.deadline_utc_ms)) {
        return validated_result(failed_result(job, base::ErrorCode::kCancelled));
    }
    std::vector<std::unique_ptr<ports::IResolvedSecret>> secrets;
    {
        ScopedStage stage(task_log.get(), "resolve_credentials");
        auto resolved = resolve_restore_secrets(job, context.credentials, cancellation);
        if (!resolved) {
            stage.fail(resolved.error(), "resolve_secret",
                       restore_hint_for(resolved.error().code, resolved.error().message));
            return validated_result(failed_result(job, resolved.error().code, &resolved.error()));
        }
        secrets = std::move(resolved).value();
    }
    std::unique_ptr<adapters::personal_archive::PersonalFileArchiveChainReader> reader;
    {
        ScopedStage stage(task_log.get(), "open_chain");
        adapters::personal_archive::ArchiveChainOpenRequest open_request;
        open_request.maximum_chain_depth = contracts::kMaximumFileChainDepth;
        open_request.layers.reserve(job.source_refs.size());
        for (std::size_t index = 0; index < job.source_refs.size(); ++index) {
            adapters::personal_archive::ArchiveOpenRequest layer;
            layer.source = path_from_utf8(job.source_refs[index]);
            layer.password = secrets[index]->view();
            layer.maximum_chunk_payload_size = options.memory_budget_bytes;
            layer.maximum_chunk_logical_size = options.memory_budget_bytes;
            open_request.layers.push_back(std::move(layer));
        }
        auto opened =
            adapters::personal_archive::PersonalFileArchiveChainReader::open(open_request);
        if (!opened) {
            stage.fail(opened.error(), "PersonalFileArchiveChainReader::open",
                       restore_hint_for(opened.error().code, opened.error().message));
            return validated_result(failed_result(job, opened.error().code, &opened.error()));
        }
        reader = std::move(opened).value();
        stage.note_u64("entry_count", reader->entry_count());
        stage.note("index_generation", reader->index_root_digest());
    }
    auto target = parse_target_root_identity(job.file_restore_target->target_root_identity);
    if (!target) {
        return validated_result(failed_result(job, target.error().code, &target.error()));
    }
    auto root_utf16 = resolve_target_root_utf16(target.value());
    if (!root_utf16) {
        return validated_result(failed_result(job, root_utf16.error().code, &root_utf16.error()));
    }
    std::unique_ptr<adapters::windows_filesystem::WindowsFileTreeSink> sink;
    {
        ScopedStage stage(task_log.get(), "open_sink");
        adapters::windows_filesystem::WindowsFileTreeSinkOpenRequest open_request;
        open_request.target_root_utf16 = std::move(root_utf16).value();
        auto opened = adapters::windows_filesystem::WindowsFileTreeSink::open(open_request);
        if (!opened) {
            stage.fail(opened.error(), "WindowsFileTreeSink::open",
                       restore_hint_for(opened.error().code, opened.error().message));
            return validated_result(failed_result(job, opened.error().code, &opened.error()));
        }
        sink = std::move(opened).value();
    }
    auto entry_ids = parse_entry_ids(job.file_restore_target->entry_ids);
    if (!entry_ids) {
        return validated_result(failed_result(job, entry_ids.error().code, &entry_ids.error()));
    }
    pipeline::FileSetRestorePlan plan;
    plan.job_id = job.job_id;
    plan.trace_id = job.trace_id;
    plan.entry_ids = std::move(entry_ids).value();
    plan.conflict_policy = job.file_restore_target->conflict_policy;
    plan.restore_security = job.file_restore_target->restore_security;

    // Cap the I/O quantum so progress is published many times during large restores.
    // Using the full memory budget (e.g. 256 MiB) as the read buffer collapses a
    // ~100 MiB job into a single quantum → Desktop only ever sees 0% then 100%.
    constexpr std::uint32_t kRestoreProgressQuantumBytes = 4U * 1024U * 1024U;
    const auto budget = options.memory_budget_bytes == 0
                            ? static_cast<std::uint64_t>(kRestoreProgressQuantumBytes)
                            : options.memory_budget_bytes;
    plan.read_buffer_bytes = static_cast<std::uint32_t>(
        (std::min)(budget, static_cast<std::uint64_t>(kRestoreProgressQuantumBytes)));
    if (plan.read_buffer_bytes == 0) {
        plan.read_buffer_bytes = kRestoreProgressQuantumBytes;
    }
    pipeline::FileSetRestorePipeline pipeline(*reader, *sink, context.progress);
    auto summary = pipeline.run(plan, cancellation);
    if (!summary) {
        if (task_log) {
            task_log->section("Result");
            task_log->field("outcome", "failed");
            task_log->field("error", summary.error().message);
        }
        return validated_result(failed_result(job, summary.error().code, &summary.error()));
    }
    auto result = completed_result(job, summary.value());
    if (task_log) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started);
        task_log->section("Result");
        task_log->field("outcome", result.message_code);
        task_log->field_u64("conflict_policy",
                            static_cast<std::uint64_t>(plan.conflict_policy));
        task_log->field_bool("restore_security", plan.restore_security);
        task_log->field_u64("entries_requested", summary.value().stats.entries_requested);
        task_log->field_u64("entries_restored", summary.value().stats.entries_restored);
        task_log->field_u64("entries_failed", summary.value().stats.entries_failed);
        task_log->field_u64("directories_created", summary.value().directories_created);
        task_log->field_u64("files_published", summary.value().files_published);
        task_log->field_bytes("bytes_restored", summary.value().stats.bytes_restored);
        if (!summary.value().stats.stable_error_codes.empty()) {
            std::string codes;
            for (const auto& code : summary.value().stats.stable_error_codes) {
                if (!codes.empty()) {
                    codes.push_back(',');
                }
                codes.append(code);
            }
            task_log->field("stable_error_codes", codes);
        }
        task_log->field("elapsed", format_duration_ms(elapsed));
    }
    return validated_result(std::move(result));
}

} // namespace
} // namespace detail

base::Result<contracts::TaskResult> execute_personal_file_archive_restore_task(
    const contracts::JobRequest& job, const WindowsPersonalBackupTaskOptions& options,
    const WindowsPersonalBackupTaskContext& context,
    const base::CancellationToken& cancellation) {
    auto valid = detail::validate_task(job, options);
    if (!valid) {
        return base::Result<contracts::TaskResult>::failure(valid.error());
    }
    return detail::run_file_set_restore(job, options, context, cancellation);
}

} // namespace aegra::apps::worker
