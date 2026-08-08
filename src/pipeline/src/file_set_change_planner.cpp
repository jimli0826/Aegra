#include "aegra/pipeline/file_set_change_planner.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::pipeline {
namespace {

constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());
constexpr std::uint32_t kParentListPageSize = 256;

[[nodiscard]] base::Error err(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] base::Result<std::uint64_t> checked_add(const std::uint64_t left,
                                                      const std::uint64_t right) {
    if (left > kMaximumWireInteger || right > kMaximumWireInteger ||
        right > kMaximumWireInteger - left) {
        return base::Result<std::uint64_t>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup counter overflow"));
    }
    return base::Result<std::uint64_t>::success(left + right);
}

/// Compact parent record: identity key + main stream reference (no full FileEntryDesc).
struct ParentIndexRecord final {
    std::string volume_identity;
    std::array<std::byte, contracts::kStableFileIdBytes> file_id{};
    contracts::FileEntryKind kind{contracts::FileEntryKind::kFile};
    std::uint64_t logical_size{0};
    std::uint64_t write_time{0};
    std::uint32_t main_stream_index{0};
};

[[nodiscard]] bool identity_less(const ParentIndexRecord& left,
                                 const ParentIndexRecord& right) noexcept {
    if (left.volume_identity != right.volume_identity) {
        return left.volume_identity < right.volume_identity;
    }
    return left.file_id < right.file_id;
}

[[nodiscard]] bool identity_equal(const ParentIndexRecord& left,
                                  const contracts::StableFileIdentity& right) noexcept {
    return left.volume_identity == right.volume_identity && left.file_id == right.file_id;
}

[[nodiscard]] bool hint_identity_less(const contracts::FileChangeHint& left,
                                      const contracts::FileChangeHint& right) noexcept {
    if (left.identity.volume_identity != right.identity.volume_identity) {
        return left.identity.volume_identity < right.identity.volume_identity;
    }
    return left.identity.file_id < right.identity.file_id;
}

[[nodiscard]] int reason_rank(const contracts::FileChangeReason reason) noexcept {
    switch (reason) {
    case contracts::FileChangeReason::kAmbiguous:
        return 5;
    case contracts::FileChangeReason::kContent:
        return 4;
    case contracts::FileChangeReason::kNamespace:
        return 3;
    case contracts::FileChangeReason::kMetadata:
        return 2;
    case contracts::FileChangeReason::kNone:
        return 1;
    }
    return 5;
}

[[nodiscard]] contracts::FileChangeReason
merge_reason(const contracts::FileChangeReason left,
             const contracts::FileChangeReason right) noexcept {
    return reason_rank(left) >= reason_rank(right) ? left : right;
}

/// Sort + coalesce change hints by identity (bounded by input size, not full tree).
[[nodiscard]] std::vector<contracts::FileChangeHint>
coalesce_hints(std::vector<contracts::FileChangeHint> hints) {
    if (hints.empty()) {
        return hints;
    }
    std::sort(hints.begin(), hints.end(), hint_identity_less);
    std::vector<contracts::FileChangeHint> merged;
    merged.reserve(hints.size());
    for (auto& hint : hints) {
        if (!merged.empty() && merged.back().identity == hint.identity) {
            merged.back().reason = merge_reason(merged.back().reason, hint.reason);
        } else {
            merged.push_back(std::move(hint));
        }
    }
    return merged;
}

[[nodiscard]] contracts::FileChangeReason
lookup_hint(const std::vector<contracts::FileChangeHint>& sorted_hints,
            const contracts::StableFileIdentity& identity) noexcept {
    contracts::FileChangeHint key;
    key.identity = identity;
    const auto found =
        std::lower_bound(sorted_hints.begin(), sorted_hints.end(), key, hint_identity_less);
    if (found == sorted_hints.end() || found->identity != identity) {
        return contracts::FileChangeReason::kNone;
    }
    return found->reason;
}

[[nodiscard]] base::Result<std::optional<ParentIndexRecord>>
record_from_entry(const contracts::FileEntryDesc& entry) {
    if (contracts::is_null_stable_file_identity(entry.stable_identity)) {
        return base::Result<std::optional<ParentIndexRecord>>::success(std::nullopt);
    }
    ParentIndexRecord record;
    record.volume_identity = entry.stable_identity.volume_identity;
    record.file_id = entry.stable_identity.file_id;
    record.kind = entry.kind;
    record.logical_size = entry.logical_size;
    record.write_time = entry.write_time;
    if (entry.kind == contracts::FileEntryKind::kFile) {
        if (entry.streams.size() != 1 ||
            entry.streams.front().stream_kind != contracts::FileStreamKind::kMain) {
            return base::Result<std::optional<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.parent_stream_invalid"));
        }
        record.main_stream_index = entry.streams.front().stream_index;
        if (record.main_stream_index == 0) {
            return base::Result<std::optional<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.parent_stream_invalid"));
        }
    }
    return base::Result<std::optional<ParentIndexRecord>>::success(std::move(record));
}

[[nodiscard]] base::Result<std::vector<ParentIndexRecord>>
build_parent_index(ports::IFileRecoveryPointReader& parent, const std::size_t budget_bytes,
                   const base::CancellationToken cancellation) {
    if (budget_bytes < sizeof(ParentIndexRecord)) {
        return base::Result<std::vector<ParentIndexRecord>>::failure(
            err(base::ErrorCode::kInsufficientSpace, "file_backup.parent_index_budget"));
    }
    const auto max_records = budget_bytes / sizeof(ParentIndexRecord);
    std::vector<ParentIndexRecord> index;
    index.reserve((std::min)(max_records, static_cast<std::size_t>(parent.entry_count())));

    // BFS over parent directory graph via list_children; describe only as needed.
    std::vector<std::uint64_t> queue;
    queue.push_back(0); // root parent_entry_id for top-level entries
    std::size_t head = 0;
    while (head < queue.size()) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kCancelled, "file backup parent index cancelled"));
        }
        const auto parent_id = queue[head++];
        std::optional<std::string> token;
        while (true) {
            auto page = parent.list_children(parent_id, kParentListPageSize, token, cancellation);
            if (!page) {
                return base::Result<std::vector<ParentIndexRecord>>::failure(page.error());
            }
            for (const auto& summary : page.value().items) {
                std::uint64_t entry_id = 0;
                try {
                    entry_id = std::stoull(summary.entry_id);
                } catch (...) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(
                        err(base::ErrorCode::kInvalidArgument, "file_backup.parent_entry_id"));
                }
                auto described = parent.describe_entry(entry_id, cancellation);
                if (!described) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(described.error());
                }
                auto record = record_from_entry(described.value());
                if (!record) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(record.error());
                }
                if (record.value().has_value()) {
                    if (index.size() >= max_records) {
                        return base::Result<std::vector<ParentIndexRecord>>::failure(
                            err(base::ErrorCode::kInsufficientSpace,
                                "file_backup.parent_index_budget"));
                    }
                    index.push_back(std::move(record.value()).value());
                }
                if (described.value().kind == contracts::FileEntryKind::kDirectory) {
                    queue.push_back(entry_id);
                }
            }
            if (!page.value().continuation_token.has_value()) {
                break;
            }
            token = page.value().continuation_token;
        }
    }

    std::sort(index.begin(), index.end(), identity_less);
    // Reject duplicate stable identities in parent (corrupt parent index).
    for (std::size_t i = 1; i < index.size(); ++i) {
        if (!identity_less(index[i - 1], index[i]) && !identity_less(index[i], index[i - 1])) {
            return base::Result<std::vector<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.parent_identity_duplicate"));
        }
    }
    return base::Result<std::vector<ParentIndexRecord>>::success(std::move(index));
}

[[nodiscard]] const ParentIndexRecord*
find_parent(const std::vector<ParentIndexRecord>& index,
            const contracts::StableFileIdentity& identity) noexcept {
    ParentIndexRecord key;
    key.volume_identity = identity.volume_identity;
    key.file_id = identity.file_id;
    const auto found = std::lower_bound(index.begin(), index.end(), key, identity_less);
    if (found == index.end() || !identity_equal(*found, identity)) {
        return nullptr;
    }
    return &*found;
}

[[nodiscard]] FileEntryChangeClass
classify_file(const contracts::FileEntryDesc& current, const ParentIndexRecord* parent,
              const contracts::FileChangeReason reason) noexcept {
    if (parent == nullptr) {
        return FileEntryChangeClass::kNew;
    }
    if (parent->kind != current.kind) {
        return FileEntryChangeClass::kNew;
    }
    // Size change always forces content (never trust size match alone as unchanged).
    if (current.logical_size != parent->logical_size) {
        return FileEntryChangeClass::kContent;
    }
    switch (reason) {
    case contracts::FileChangeReason::kContent:
        return FileEntryChangeClass::kContent;
    case contracts::FileChangeReason::kAmbiguous:
        return FileEntryChangeClass::kAmbiguous;
    case contracts::FileChangeReason::kMetadata:
        return FileEntryChangeClass::kMetadataOnly;
    case contracts::FileChangeReason::kNamespace:
        return FileEntryChangeClass::kRenameMove;
    case contracts::FileChangeReason::kNone:
        return FileEntryChangeClass::kUnchanged;
    }
    return FileEntryChangeClass::kAmbiguous;
}

[[nodiscard]] bool class_uses_local(const FileEntryChangeClass change_class) noexcept {
    switch (change_class) {
    case FileEntryChangeClass::kNew:
    case FileEntryChangeClass::kContent:
    case FileEntryChangeClass::kAmbiguous:
        return true;
    case FileEntryChangeClass::kMetadataOnly:
    case FileEntryChangeClass::kRenameMove:
    case FileEntryChangeClass::kUnchanged:
        return false;
    }
    return true;
}

[[nodiscard]] base::Result<void>
assign_full_local_streams(std::vector<contracts::FileEntryDesc>& entries,
                          std::vector<FileLocalStreamWork>& work, std::uint64_t& stream_count,
                          std::uint64_t& logical_bytes) {
    std::uint32_t next_stream_index = 1;
    for (std::size_t entry_pos = 0; entry_pos < entries.size(); ++entry_pos) {
        auto& entry = entries[entry_pos];
        if (entry.entry_id == 0 || !contracts::is_known_file_entry_kind(entry.kind)) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_object"));
        }
        if (entry.kind == contracts::FileEntryKind::kDirectory) {
            if (!entry.streams.empty() || entry.logical_size != 0) {
                return base::Result<void>::failure(
                    err(base::ErrorCode::kInvalidArgument, "directory entry is invalid"));
            }
            continue;
        }
        if (entry.streams.size() != 1 ||
            entry.streams.front().stream_kind != contracts::FileStreamKind::kMain ||
            !entry.streams.front().name.bytes.empty()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_ads"));
        }
        auto& stream = entry.streams.front();
        if (stream.stream_index == 0) {
            stream.stream_index = next_stream_index++;
        } else {
            next_stream_index = (std::max)(next_stream_index, stream.stream_index + 1);
        }
        stream.content_storage = contracts::FileContentStorage::kLocal;
        stream.parent_stream_index = 0;
        stream.extents.clear();
        if (entry.logical_size != stream.logical_size) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file logical_size mismatch"));
        }
        auto counted = checked_add(stream_count, 1);
        if (!counted) {
            return base::Result<void>::failure(counted.error());
        }
        stream_count = counted.value();
        auto bytes = checked_add(logical_bytes, stream.logical_size);
        if (!bytes) {
            return base::Result<void>::failure(bytes.error());
        }
        logical_bytes = bytes.value();
        if (stream.logical_size == 0) {
            continue;
        }
        FileLocalStreamWork item;
        item.entry_id = entry.entry_id;
        item.stream_index = stream.stream_index;
        item.entry_pos = entry_pos;
        item.stream_pos = 0;
        work.push_back(item);
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
assign_incremental_streams(std::vector<contracts::FileEntryDesc>& entries,
                           const std::vector<ParentIndexRecord>& parent_index,
                           const std::vector<contracts::FileChangeHint>& hints,
                           std::vector<FileLocalStreamWork>& work, std::uint64_t& stream_count,
                           std::uint64_t& logical_bytes,
                           const base::CancellationToken cancellation) {
    std::uint32_t next_stream_index = 1;
    for (std::size_t entry_pos = 0; entry_pos < entries.size(); ++entry_pos) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file backup plan cancelled"));
        }
        auto& entry = entries[entry_pos];
        if (entry.entry_id == 0 || !contracts::is_known_file_entry_kind(entry.kind)) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_object"));
        }
        if (entry.kind == contracts::FileEntryKind::kDirectory) {
            if (!entry.streams.empty() || entry.logical_size != 0) {
                return base::Result<void>::failure(
                    err(base::ErrorCode::kInvalidArgument, "directory entry is invalid"));
            }
            continue;
        }
        if (entry.streams.size() != 1 ||
            entry.streams.front().stream_kind != contracts::FileStreamKind::kMain ||
            !entry.streams.front().name.bytes.empty()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_ads"));
        }
        auto& stream = entry.streams.front();
        if (entry.logical_size != stream.logical_size) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "file logical_size mismatch"));
        }
        if (stream.stream_index == 0) {
            stream.stream_index = next_stream_index++;
        } else {
            next_stream_index = (std::max)(next_stream_index, stream.stream_index + 1);
        }

        const auto* parent = find_parent(parent_index, entry.stable_identity);
        const auto reason = lookup_hint(hints, entry.stable_identity);
        const auto change_class = classify_file(entry, parent, reason);
        stream.extents.clear();

        if (class_uses_local(change_class) || parent == nullptr || parent->main_stream_index == 0) {
            stream.content_storage = contracts::FileContentStorage::kLocal;
            stream.parent_stream_index = 0;
            auto counted = checked_add(stream_count, 1);
            if (!counted) {
                return base::Result<void>::failure(counted.error());
            }
            stream_count = counted.value();
            auto bytes = checked_add(logical_bytes, stream.logical_size);
            if (!bytes) {
                return base::Result<void>::failure(bytes.error());
            }
            logical_bytes = bytes.value();
            if (stream.logical_size > 0) {
                FileLocalStreamWork item;
                item.entry_id = entry.entry_id;
                item.stream_index = stream.stream_index;
                item.entry_pos = entry_pos;
                item.stream_pos = 0;
                work.push_back(item);
            }
        } else {
            // Metadata / rename / unchanged: reference direct parent main stream.
            stream.content_storage = contracts::FileContentStorage::kParent;
            stream.parent_stream_index = parent->main_stream_index;
            stream.logical_size = parent->logical_size;
            entry.logical_size = parent->logical_size;
            auto counted = checked_add(stream_count, 1);
            if (!counted) {
                return base::Result<void>::failure(counted.error());
            }
            stream_count = counted.value();
            // Parent-referenced bytes are not stored in this layer; logical_bytes still
            // counts tip-visible size for progress (matches Full semantics for totals).
            auto bytes = checked_add(logical_bytes, stream.logical_size);
            if (!bytes) {
                return base::Result<void>::failure(bytes.error());
            }
            logical_bytes = bytes.value();
        }
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<FileSetChangePlanResult>
plan_file_set_streams(std::vector<contracts::FileEntryDesc> current_entries,
                      const FileSetChangePlannerRequest& request,
                      const base::CancellationToken cancellation) {
    if (current_entries.empty()) {
        return base::Result<FileSetChangePlanResult>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup selection is empty"));
    }
    FileSetChangePlanResult result;
    result.entries = std::move(current_entries);

    if (request.effective_type != contracts::BackupType::kIncremental) {
        auto planned = assign_full_local_streams(result.entries, result.local_streams,
                                                 result.stream_count, result.logical_bytes);
        if (!planned) {
            return base::Result<FileSetChangePlanResult>::failure(planned.error());
        }
        return base::Result<FileSetChangePlanResult>::success(std::move(result));
    }

    if (request.parent_reader == nullptr) {
        return base::Result<FileSetChangePlanResult>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_backup.parent_required"));
    }
    auto parent_index =
        build_parent_index(*request.parent_reader, request.parent_index_budget_bytes, cancellation);
    if (!parent_index) {
        return base::Result<FileSetChangePlanResult>::failure(parent_index.error());
    }
    auto hints = coalesce_hints(request.change_hints);
    auto planned =
        assign_incremental_streams(result.entries, parent_index.value(), hints, result.local_streams,
                                   result.stream_count, result.logical_bytes, cancellation);
    if (!planned) {
        return base::Result<FileSetChangePlanResult>::failure(planned.error());
    }
    return base::Result<FileSetChangePlanResult>::success(std::move(result));
}

} // namespace aegra::pipeline
