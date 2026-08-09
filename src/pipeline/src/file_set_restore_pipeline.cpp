#include "aegra/pipeline/file_set_restore_pipeline.h"

#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"

#include <algorithm>
#include <limits>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegra::pipeline {
namespace {

constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

[[nodiscard]] base::Error err(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

/// Dedup + hard cap (contracts::kMaximumPartialRestoreErrorCodes) so partial stats stay valid.
void record_stable_error(contracts::PartialRestoreStats& stats, const std::string_view code) {
    if (code.empty() ||
        stats.stable_error_codes.size() >= contracts::kMaximumPartialRestoreErrorCodes) {
        return;
    }
    for (const auto& existing : stats.stable_error_codes) {
        if (existing == code) {
            return;
        }
    }
    stats.stable_error_codes.emplace_back(code);
}

[[nodiscard]] base::Result<std::uint64_t> checked_add(const std::uint64_t left,
                                                      const std::uint64_t right) {
    if (left > kMaximumWireInteger || right > kMaximumWireInteger ||
        right > kMaximumWireInteger - left) {
        return base::Result<std::uint64_t>::failure(
            err(base::ErrorCode::kInvalidArgument, "file restore counter overflow"));
    }
    return base::Result<std::uint64_t>::success(left + right);
}

void publish(ports::IProgressSink* sink, const FileSetRestorePlan& plan,
             const contracts::TaskPhase phase, const FileSetRestoreSummary& summary,
             const char* message) {
    if (sink == nullptr) {
        return;
    }
    contracts::TaskProgress progress;
    progress.job_id = plan.job_id;
    progress.trace_id = plan.trace_id;
    progress.phase = phase;
    // Mirror file backup: Desktop percent = processed_bytes / logical_bytes.
    // Completed must satisfy processed == logical when logical is set (contracts).
    if (summary.progress_logical_bytes > 0) {
        progress.logical_bytes = summary.progress_logical_bytes;
        if (phase == contracts::TaskPhase::kCompleted) {
            progress.processed_bytes = summary.progress_logical_bytes;
            progress.stored_bytes = summary.stats.bytes_restored;
        } else {
            // Cap at denominator so partial mid-stream never exceeds planned total.
            progress.processed_bytes =
                (std::min)(summary.stats.bytes_restored, summary.progress_logical_bytes);
            progress.stored_bytes = progress.processed_bytes;
        }
    } else {
        progress.processed_bytes = summary.stats.bytes_restored;
        progress.stored_bytes = summary.stats.bytes_restored;
    }
    progress.discovered_entries = summary.stats.entries_requested;
    progress.processed_entries = summary.stats.entries_restored;
    progress.message_code = message;
    sink->publish(std::move(progress));
}

[[nodiscard]] base::Result<std::uint64_t>
sum_file_logical_bytes(const std::vector<contracts::FileEntryDesc>& entries) {
    std::uint64_t total = 0;
    for (const auto& entry : entries) {
        if (entry.kind != contracts::FileEntryKind::kFile) {
            continue;
        }
        const auto size = entry.streams.size() == 1 ? entry.streams.front().logical_size
                                                    : entry.logical_size;
        auto next = checked_add(total, size);
        if (!next) {
            return next;
        }
        total = next.value();
    }
    return base::Result<std::uint64_t>::success(total);
}

[[nodiscard]] base::Result<void> validate_plan(const FileSetRestorePlan& plan) {
    if (plan.job_id.empty() || plan.trace_id.empty() || plan.read_buffer_bytes == 0) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file restore plan is incomplete"));
    }
    if (!contracts::is_known_file_conflict_policy(plan.conflict_policy)) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file restore conflict policy is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
describe_into(ports::IFileRecoveryPointReader& reader, const std::uint64_t entry_id,
              std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id,
              std::vector<contracts::FileEntryDesc>& selected,
              std::vector<std::uint64_t>* expand_directories,
              const base::CancellationToken& cancellation) {
    if (by_id.contains(entry_id)) {
        return base::Result<void>::success();
    }
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kCancelled, "file restore preflight cancelled"));
    }
    auto entry = reader.describe_entry(entry_id, cancellation);
    if (!entry) {
        return base::Result<void>::failure(entry.error());
    }
    if (expand_directories != nullptr &&
        entry.value().kind == contracts::FileEntryKind::kDirectory) {
        expand_directories->push_back(entry_id);
    }
    by_id.emplace(entry_id, entry.value());
    selected.push_back(std::move(entry).value());
    return base::Result<void>::success();
}

/// Breadth-first: for each directory id, page all children into the selection set.
[[nodiscard]] base::Result<void>
expand_directory_descendants(ports::IFileRecoveryPointReader& reader,
                             std::vector<std::uint64_t>& directory_queue,
                             std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id,
                             std::vector<contracts::FileEntryDesc>& selected,
                             const base::CancellationToken& cancellation) {
    while (!directory_queue.empty()) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file restore preflight cancelled"));
        }
        const auto parent = directory_queue.back();
        directory_queue.pop_back();
        std::optional<std::string> token;
        do {
            auto page = reader.list_children(parent, 256, token, cancellation);
            if (!page) {
                return base::Result<void>::failure(page.error());
            }
            for (const auto& summary : page.value().items) {
                const auto child_id = std::stoull(summary.entry_id);
                auto added =
                    describe_into(reader, child_id, by_id, selected, &directory_queue, cancellation);
                if (!added) {
                    return added;
                }
            }
            token = page.value().continuation_token;
        } while (token.has_value());
    }
    return base::Result<void>::success();
}

/// Path materialization needs every ancestor between a selected entry and root 0.
/// visited/depth guards refuse parent cycles (defense in depth; Reader already rejects C07).
[[nodiscard]] base::Result<void>
include_path_ancestors(ports::IFileRecoveryPointReader& reader,
                       std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id,
                       std::vector<contracts::FileEntryDesc>& selected,
                       const base::CancellationToken& cancellation) {
    std::vector<std::uint64_t> seeds;
    seeds.reserve(selected.size());
    for (const auto& entry : selected) {
        seeds.push_back(entry.entry_id);
    }
    for (const auto seed_id : seeds) {
        const auto found = by_id.find(seed_id);
        if (found == by_id.end()) {
            continue;
        }
        std::unordered_set<std::uint64_t> seen{seed_id};
        auto parent = found->second.parent_entry_id;
        std::uint32_t hops = 0;
        while (parent != 0) {
            if (!seen.insert(parent).second) {
                return base::Result<void>::failure(
                    err(base::ErrorCode::kCorruptData, "format.corrupt_index"));
            }
            ++hops;
            if (hops > contracts::kMaximumFileDirectoryDepth) {
                return base::Result<void>::failure(
                    err(base::ErrorCode::kCorruptData, "format.corrupt_index"));
            }
            if (by_id.contains(parent)) {
                parent = by_id[parent].parent_entry_id;
                continue;
            }
            auto added =
                describe_into(reader, parent, by_id, selected, nullptr, cancellation);
            if (!added) {
                return added;
            }
            parent = by_id[parent].parent_entry_id;
        }
    }
    return base::Result<void>::success();
}

/// Selection closure (FILE_SET_BACKUP_RESTORE §12): directories include all reachable
/// descendants; ancestors are included so relative paths and directory skeleton work.
[[nodiscard]] base::Result<std::vector<contracts::FileEntryDesc>>
load_selected_entries(ports::IFileRecoveryPointReader& reader, const FileSetRestorePlan& plan,
                      const base::CancellationToken& cancellation) {
    std::vector<contracts::FileEntryDesc> selected;
    std::unordered_map<std::uint64_t, contracts::FileEntryDesc> by_id;
    std::vector<std::uint64_t> directory_queue;
    if (!plan.entry_ids.empty()) {
        selected.reserve(plan.entry_ids.size());
        for (const auto entry_id : plan.entry_ids) {
            if (entry_id == 0) {
                return base::Result<std::vector<contracts::FileEntryDesc>>::failure(
                    err(base::ErrorCode::kInvalidArgument, "file restore entry_id 0 is invalid"));
            }
            auto added =
                describe_into(reader, entry_id, by_id, selected, &directory_queue, cancellation);
            if (!added) {
                return base::Result<std::vector<contracts::FileEntryDesc>>::failure(added.error());
            }
        }
        auto expanded =
            expand_directory_descendants(reader, directory_queue, by_id, selected, cancellation);
        if (!expanded) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(expanded.error());
        }
        auto ancestors = include_path_ancestors(reader, by_id, selected, cancellation);
        if (!ancestors) {
            return base::Result<std::vector<contracts::FileEntryDesc>>::failure(ancestors.error());
        }
        return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(selected));
    }
    // Full tree: walk from parent 0 with pagination (same expansion machinery).
    directory_queue.push_back(0);
    auto expanded =
        expand_directory_descendants(reader, directory_queue, by_id, selected, cancellation);
    if (!expanded) {
        return base::Result<std::vector<contracts::FileEntryDesc>>::failure(expanded.error());
    }
    return base::Result<std::vector<contracts::FileEntryDesc>>::success(std::move(selected));
}

[[nodiscard]] std::uint32_t
entry_depth(const contracts::FileEntryDesc& entry,
            const std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id) {
    std::uint32_t depth = 0;
    std::uint64_t current = entry.entry_id;
    std::unordered_set<std::uint64_t> seen;
    while (current != 0 && seen.insert(current).second) {
        const auto found = by_id.find(current);
        if (found == by_id.end()) {
            break;
        }
        ++depth;
        current = found->second.parent_entry_id;
    }
    return depth;
}

/// Defense-in-depth: refuse unsupported entry semantics before any Sink mutation (FI0).
[[nodiscard]] base::Result<void>
preflight_entries(ports::IFileTreeSink& sink, const std::vector<contracts::FileEntryDesc>& entries,
                  const FileSetRestorePlan& plan, const base::CancellationToken& cancellation) {
    auto caps = sink.capabilities(cancellation);
    if (!caps) {
        return base::Result<void>::failure(caps.error());
    }
    for (const auto& entry : entries) {
        auto validated = contracts::validate_file_entry_desc(entry);
        if (!validated) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCorruptData, "file_restore.unsupported_archive_semantics"));
        }
        if (plan.restore_security && !entry.platform_metadata.empty() &&
            !caps.value().supports_security_descriptor) {
            return base::Result<void>::failure(err(
                base::ErrorCode::kInvalidArgument, "file_restore.target_capability_missing"));
        }
        if (entry.kind == contracts::FileEntryKind::kFile &&
            entry.logical_size > caps.value().maximum_file_size_bytes) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_restore.target_file_too_large"));
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<contracts::EncodedName>>
path_for_entry(const contracts::FileEntryDesc& entry,
               const std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id) {
    std::vector<contracts::EncodedName> components;
    std::uint64_t current = entry.entry_id;
    std::unordered_set<std::uint64_t> seen;
    while (current != 0) {
        if (!seen.insert(current).second) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                err(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        if (seen.size() > contracts::kMaximumFileDirectoryDepth) {
            return base::Result<std::vector<contracts::EncodedName>>::failure(
                err(base::ErrorCode::kCorruptData, "format.corrupt_index"));
        }
        const auto found = by_id.find(current);
        if (found == by_id.end()) {
            break;
        }
        components.push_back(found->second.name);
        current = found->second.parent_entry_id;
    }
    std::ranges::reverse(components);
    return base::Result<std::vector<contracts::EncodedName>>::success(std::move(components));
}

[[nodiscard]] base::Result<void>
restore_file_content(ports::IFileRecoveryPointReader& reader, ports::IStagedFileWriter& writer,
                     const contracts::FileEntryDesc& entry, const FileSetRestorePlan& plan,
                     FileSetRestoreSummary& summary, ports::IProgressSink* progress,
                     const base::CancellationToken& cancellation) {
    if (entry.streams.size() != 1) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kCorruptData, "file_restore.unsupported_archive_semantics"));
    }
    const auto& stream = entry.streams.front();
    std::vector<std::byte> buffer(plan.read_buffer_bytes);
    std::uint64_t offset = 0;
    while (offset < stream.logical_size) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file restore cancelled"));
        }
        const auto want = static_cast<std::uint64_t>(
            (std::min)(static_cast<std::uint64_t>(buffer.size()), stream.logical_size - offset));
        ports::FileStreamReadRequest request;
        request.stream_index = stream.stream_index;
        request.offset = offset;
        request.size = want;
        auto read =
            reader.read_stream(request, std::span(buffer).first(static_cast<std::size_t>(want)),
                               cancellation);
        if (!read) {
            return base::Result<void>::failure(read.error());
        }
        if (read.value() == 0) {
            break;
        }
        auto written = writer.write(
            offset, std::span<const std::byte>(buffer.data(), read.value()), cancellation);
        if (!written) {
            return written;
        }
        offset += read.value();
        auto bytes = checked_add(summary.stats.bytes_restored, read.value());
        if (!bytes) {
            return base::Result<void>::failure(bytes.error());
        }
        summary.stats.bytes_restored = bytes.value();
        // Publish after each quantum so Desktop can poll live percent while writing.
        publish(progress, plan, contracts::TaskPhase::kWriting, summary, "file_restore.writing");
    }
    return base::Result<void>::success();
}

struct PartitionedEntries final {
    std::vector<contracts::FileEntryDesc> directories;
    std::vector<contracts::FileEntryDesc> files;
    std::unordered_map<std::uint64_t, contracts::FileEntryDesc> by_id;
};

struct RestoreMutationContext final {
    ports::IFileRecoveryPointReader& reader;
    ports::IFileTreeSink& sink;
    const FileSetRestorePlan& plan;
    FileSetRestoreSummary& summary;
    ports::IProgressSink* progress{nullptr};
    const std::unordered_map<std::uint64_t, contracts::FileEntryDesc>& by_id;
};

[[nodiscard]] PartitionedEntries
partition_selected(std::vector<contracts::FileEntryDesc> selected) {
    PartitionedEntries parts;
    for (auto& entry : selected) {
        parts.by_id.emplace(entry.entry_id, entry);
        if (entry.kind == contracts::FileEntryKind::kDirectory) {
            parts.directories.push_back(std::move(entry));
        } else {
            parts.files.push_back(std::move(entry));
        }
    }
    std::ranges::sort(parts.directories, [&parts](const auto& left, const auto& right) {
        const auto left_depth = entry_depth(left, parts.by_id);
        const auto right_depth = entry_depth(right, parts.by_id);
        if (left_depth != right_depth) {
            return left_depth < right_depth;
        }
        return left.entry_id < right.entry_id;
    });
    return parts;
}

[[nodiscard]] base::Result<std::unordered_set<std::uint64_t>>
create_directories(RestoreMutationContext& ctx,
                   const std::vector<contracts::FileEntryDesc>& directories,
                   const base::CancellationToken& cancellation) {
    std::unordered_set<std::uint64_t> created;
    for (const auto& entry : directories) {
        if (cancellation.stop_requested()) {
            return base::Result<std::unordered_set<std::uint64_t>>::failure(
                err(base::ErrorCode::kCancelled, "file restore cancelled"));
        }
        auto path = path_for_entry(entry, ctx.by_id);
        if (!path) {
            return base::Result<std::unordered_set<std::uint64_t>>::failure(path.error());
        }
        auto ok = ctx.sink.create_directory(path.value(), cancellation);
        if (!ok) {
            ++ctx.summary.stats.entries_failed;
            record_stable_error(ctx.summary.stats, "file_restore.directory_create_failed");
            continue;
        }
        created.insert(entry.entry_id);
        ++ctx.summary.directories_created;
        ++ctx.summary.stats.entries_restored;
        publish(ctx.progress, ctx.plan, contracts::TaskPhase::kWriting, ctx.summary,
                "file_restore.writing");
    }
    return base::Result<std::unordered_set<std::uint64_t>>::success(std::move(created));
}

[[nodiscard]] base::Result<void>
restore_one_file(RestoreMutationContext& ctx, const contracts::FileEntryDesc& entry,
                 const base::CancellationToken& cancellation) {
    auto path = path_for_entry(entry, ctx.by_id);
    if (!path) {
        return base::Result<void>::failure(path.error());
    }
    auto writer = ctx.sink.begin_file(path.value(), entry.logical_size, cancellation);
    if (!writer) {
        ++ctx.summary.stats.entries_failed;
        record_stable_error(ctx.summary.stats, "file_restore.file_create_failed");
        return base::Result<void>::success();
    }
    auto content = restore_file_content(ctx.reader, *writer.value(), entry, ctx.plan, ctx.summary,
                                        ctx.progress, cancellation);
    if (!content) {
        writer.value()->abort();
        ++ctx.summary.stats.entries_failed;
        record_stable_error(ctx.summary.stats, "file_restore.file_write_failed");
        publish(ctx.progress, ctx.plan, contracts::TaskPhase::kWriting, ctx.summary,
                "file_restore.writing");
        return base::Result<void>::success();
    }
    contracts::FileEntryDesc meta_entry = entry;
    if (!ctx.plan.restore_security) {
        meta_entry.platform_metadata.clear();
        meta_entry.flags &= ~contracts::kEntryFlagHasSecurity;
    }
    auto meta = writer.value()->apply_metadata(meta_entry, cancellation);
    if (!meta) {
        writer.value()->abort();
        ++ctx.summary.stats.entries_failed;
        record_stable_error(ctx.summary.stats, "file_restore.metadata_failed");
        publish(ctx.progress, ctx.plan, contracts::TaskPhase::kWriting, ctx.summary,
                "file_restore.writing");
        return base::Result<void>::success();
    }
    auto published = writer.value()->publish(ctx.plan.conflict_policy, cancellation);
    if (!published) {
        writer.value()->abort();
        ++ctx.summary.stats.entries_failed;
        const auto& message = published.error().message;
        if (message.starts_with("file_restore.")) {
            record_stable_error(ctx.summary.stats, message);
        } else {
            record_stable_error(ctx.summary.stats, "file_restore.publish_failed");
        }
        publish(ctx.progress, ctx.plan, contracts::TaskPhase::kWriting, ctx.summary,
                "file_restore.writing");
        return base::Result<void>::success();
    }
    ++ctx.summary.files_published;
    ++ctx.summary.stats.entries_restored;
    publish(ctx.progress, ctx.plan, contracts::TaskPhase::kWriting, ctx.summary,
            "file_restore.writing");
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
restore_files(RestoreMutationContext& ctx, const std::vector<contracts::FileEntryDesc>& files,
              const base::CancellationToken& cancellation) {
    for (const auto& entry : files) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file restore cancelled"));
        }
        auto one = restore_one_file(ctx, entry, cancellation);
        if (!one) {
            return one;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
apply_directory_metadata(RestoreMutationContext& ctx,
                         const std::vector<contracts::FileEntryDesc>& directories,
                         const std::unordered_set<std::uint64_t>& created,
                         const base::CancellationToken& cancellation) {
    for (const auto& entry : directories) {
        if (!created.contains(entry.entry_id)) {
            continue;
        }
        auto path = path_for_entry(entry, ctx.by_id);
        if (!path) {
            return base::Result<void>::failure(path.error());
        }
        contracts::FileEntryDesc meta_entry = entry;
        if (!ctx.plan.restore_security) {
            meta_entry.platform_metadata.clear();
            meta_entry.flags &= ~contracts::kEntryFlagHasSecurity;
        }
        auto meta = ctx.sink.apply_directory_metadata(path.value(), meta_entry, cancellation);
        if (!meta) {
            if (ctx.summary.stats.entries_restored > 0) {
                --ctx.summary.stats.entries_restored;
            }
            if (ctx.summary.directories_created > 0) {
                --ctx.summary.directories_created;
            }
            ++ctx.summary.stats.entries_failed;
            record_stable_error(ctx.summary.stats, "file_restore.directory_metadata_failed");
        }
    }
    return base::Result<void>::success();
}

} // namespace

FileSetRestorePipeline::FileSetRestorePipeline(ports::IFileRecoveryPointReader& reader,
                                               ports::IFileTreeSink& sink,
                                               ports::IProgressSink* progress) noexcept
    : reader_(reader), sink_(sink), progress_(progress) {}

base::Result<FileSetRestoreSummary>
FileSetRestorePipeline::run(const FileSetRestorePlan& plan,
                            const base::CancellationToken& cancellation) {
    auto validation = validate_plan(plan);
    if (!validation) {
        return base::Result<FileSetRestoreSummary>::failure(validation.error());
    }
    FileSetRestoreSummary summary;
    publish(progress_, plan, contracts::TaskPhase::kPreparing, summary, "file_restore.preflight");
    auto selected = load_selected_entries(reader_, plan, cancellation);
    if (!selected) {
        return base::Result<FileSetRestoreSummary>::failure(selected.error());
    }
    summary.stats.entries_requested = selected.value().size();
    auto planned_bytes = sum_file_logical_bytes(selected.value());
    if (!planned_bytes) {
        return base::Result<FileSetRestoreSummary>::failure(planned_bytes.error());
    }
    summary.progress_logical_bytes = planned_bytes.value();
    auto caps = preflight_entries(sink_, selected.value(), plan, cancellation);
    if (!caps) {
        return base::Result<FileSetRestoreSummary>::failure(caps.error());
    }
    auto parts = partition_selected(std::move(selected).value());
    RestoreMutationContext ctx{reader_, sink_, plan, summary, progress_, parts.by_id};
    publish(progress_, plan, contracts::TaskPhase::kWriting, summary, "file_restore.writing");
    auto created = create_directories(ctx, parts.directories, cancellation);
    if (!created) {
        return base::Result<FileSetRestoreSummary>::failure(created.error());
    }
    auto files = restore_files(ctx, parts.files, cancellation);
    if (!files) {
        return base::Result<FileSetRestoreSummary>::failure(files.error());
    }
    auto meta = apply_directory_metadata(ctx, parts.directories, created.value(), cancellation);
    if (!meta) {
        return base::Result<FileSetRestoreSummary>::failure(meta.error());
    }
    auto flushed = sink_.flush(cancellation);
    if (!flushed) {
        return base::Result<FileSetRestoreSummary>::failure(flushed.error());
    }
    publish(progress_, plan, contracts::TaskPhase::kCompleted, summary, "file_restore.completed");
    return base::Result<FileSetRestoreSummary>::success(std::move(summary));
}

} // namespace aegra::pipeline
