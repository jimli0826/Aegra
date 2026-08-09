#include "aegra/pipeline/file_set_change_planner.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstdint>
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

struct PathKey final {
    std::string selection_id;
    std::vector<contracts::EncodedName> components;
    contracts::FileEntryKind kind{contracts::FileEntryKind::kFile};
};

struct ParentIndexRecord final {
    PathKey key;
    std::uint64_t logical_size{0};
    std::uint64_t write_time{0};
    std::uint32_t main_stream_index{0};
};

struct DirectoryPath final {
    std::uint64_t entry_id{0};
    std::string selection_id;
    std::vector<contracts::EncodedName> components;
};

[[nodiscard]] auto directory_position(std::vector<DirectoryPath>& paths,
                                      const std::uint64_t entry_id) noexcept {
    return std::lower_bound(paths.begin(), paths.end(), entry_id,
                            [](const DirectoryPath& path, const std::uint64_t candidate) {
                                return path.entry_id < candidate;
                            });
}

[[nodiscard]] auto directory_position(const std::vector<DirectoryPath>& paths,
                                      const std::uint64_t entry_id) noexcept {
    return std::lower_bound(paths.begin(), paths.end(), entry_id,
                            [](const DirectoryPath& path, const std::uint64_t candidate) {
                                return path.entry_id < candidate;
                            });
}

[[nodiscard]] int compare_name(const contracts::EncodedName& left,
                               const contracts::EncodedName& right) noexcept {
    if (left.encoding != right.encoding) {
        return left.encoding < right.encoding ? -1 : 1;
    }
    if (left.bytes == right.bytes) {
        return 0;
    }
    return std::lexicographical_compare(left.bytes.begin(), left.bytes.end(), right.bytes.begin(),
                                        right.bytes.end())
               ? -1
               : 1;
}

[[nodiscard]] int compare_path(const PathKey& left, const PathKey& right) noexcept {
    if (left.selection_id != right.selection_id) {
        return left.selection_id < right.selection_id ? -1 : 1;
    }
    const auto count = (std::min)(left.components.size(), right.components.size());
    for (std::size_t index = 0; index < count; ++index) {
        const auto compared = compare_name(left.components[index], right.components[index]);
        if (compared != 0) {
            return compared;
        }
    }
    if (left.components.size() != right.components.size()) {
        return left.components.size() < right.components.size() ? -1 : 1;
    }
    if (left.kind == right.kind) {
        return 0;
    }
    return left.kind < right.kind ? -1 : 1;
}

[[nodiscard]] bool path_less(const ParentIndexRecord& left,
                             const ParentIndexRecord& right) noexcept {
    return compare_path(left.key, right.key) < 0;
}

[[nodiscard]] std::size_t path_bytes(const std::string& selection_id,
                                     const std::vector<contracts::EncodedName>& components) {
    std::size_t total = selection_id.size();
    for (const auto& component : components) {
        if (component.bytes.size() > (std::numeric_limits<std::size_t>::max)() - total) {
            return (std::numeric_limits<std::size_t>::max)();
        }
        total += component.bytes.size();
    }
    return total;
}

[[nodiscard]] base::Result<void> charge_budget(std::size_t& used, const std::size_t amount,
                                               const std::size_t limit) {
    if (amount > limit || used > limit - amount) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInsufficientSpace, "file_backup.parent_index_budget"));
    }
    used += amount;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::uint64_t> parse_entry_id(const std::string& value) {
    try {
        std::size_t consumed = 0;
        const auto parsed = std::stoull(value, &consumed);
        if (parsed == 0 || consumed != value.size()) {
            return base::Result<std::uint64_t>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.parent_entry_id"));
        }
        return base::Result<std::uint64_t>::success(parsed);
    } catch (...) {
        return base::Result<std::uint64_t>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_backup.parent_entry_id"));
    }
}

[[nodiscard]] base::Result<ParentIndexRecord>
make_parent_record(const contracts::FileEntryDesc& entry,
                   const std::vector<contracts::EncodedName>& components) {
    ParentIndexRecord record;
    record.key.selection_id = entry.selection_id;
    record.key.components = components;
    record.key.kind = entry.kind;
    record.logical_size = entry.logical_size;
    record.write_time = entry.write_time;
    if (entry.kind != contracts::FileEntryKind::kFile) {
        return base::Result<ParentIndexRecord>::success(std::move(record));
    }
    if (entry.streams.size() != 1 ||
        entry.streams.front().stream_kind != contracts::FileStreamKind::kMain ||
        !entry.streams.front().name.bytes.empty() || entry.streams.front().stream_index == 0 ||
        entry.streams.front().logical_size != entry.logical_size) {
        return base::Result<ParentIndexRecord>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_backup.parent_stream_invalid"));
    }
    record.main_stream_index = entry.streams.front().stream_index;
    return base::Result<ParentIndexRecord>::success(std::move(record));
}

[[nodiscard]] base::Result<void>
append_parent_child(const contracts::FileEntryDesc& entry, const DirectoryPath& directory,
                    std::vector<ParentIndexRecord>& index, std::vector<DirectoryPath>& queue,
                    std::vector<std::uint64_t>& visited_entry_ids, std::size_t& used_bytes,
                    const std::size_t budget_bytes) {
    if (entry.entry_id == 0 || entry.parent_entry_id != directory.entry_id ||
        entry.selection_id.empty() ||
        (!directory.selection_id.empty() && entry.selection_id != directory.selection_id)) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kConflict, "file_backup.parent_path_invalid"));
    }
    const auto visited =
        std::lower_bound(visited_entry_ids.begin(), visited_entry_ids.end(), entry.entry_id);
    if (visited != visited_entry_ids.end() && *visited == entry.entry_id) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kConflict, "file_backup.parent_entry_duplicate"));
    }
    auto charged = charge_budget(used_bytes, sizeof(std::uint64_t), budget_bytes);
    if (!charged) {
        return charged;
    }
    visited_entry_ids.insert(visited, entry.entry_id);
    auto components = directory.components;
    components.push_back(entry.name);
    const auto bytes = path_bytes(entry.selection_id, components);
    charged = charge_budget(used_bytes, sizeof(ParentIndexRecord) + bytes, budget_bytes);
    if (!charged) {
        return charged;
    }
    auto record = make_parent_record(entry, components);
    if (!record) {
        return base::Result<void>::failure(record.error());
    }
    index.push_back(std::move(record).value());
    if (entry.kind == contracts::FileEntryKind::kDirectory) {
        charged = charge_budget(used_bytes, sizeof(DirectoryPath) + bytes, budget_bytes);
        if (!charged) {
            return charged;
        }
        DirectoryPath child;
        child.entry_id = entry.entry_id;
        child.selection_id = entry.selection_id;
        child.components = std::move(components);
        queue.push_back(std::move(child));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<ParentIndexRecord>>
build_parent_index(ports::IFileRecoveryPointReader& parent, const std::size_t budget_bytes,
                   const base::CancellationToken cancellation) {
    std::vector<ParentIndexRecord> index;
    std::vector<DirectoryPath> queue(1);
    std::vector<std::uint64_t> visited_entry_ids;
    std::size_t head = 0;
    std::size_t used_bytes = 0;
    while (head < queue.size()) {
        if (cancellation.stop_requested()) {
            return base::Result<std::vector<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kCancelled, "file backup parent index cancelled"));
        }
        const auto directory = queue[head++];
        std::optional<std::string> token;
        std::vector<std::string> visited_tokens;
        do {
            auto page =
                parent.list_children(directory.entry_id, kParentListPageSize, token, cancellation);
            if (!page) {
                return base::Result<std::vector<ParentIndexRecord>>::failure(page.error());
            }
            for (const auto& summary : page.value().items) {
                auto entry_id = parse_entry_id(summary.entry_id);
                if (!entry_id) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(entry_id.error());
                }
                auto entry = parent.describe_entry(entry_id.value(), cancellation);
                if (!entry) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(entry.error());
                }
                auto appended = append_parent_child(entry.value(), directory, index, queue,
                                                    visited_entry_ids, used_bytes, budget_bytes);
                if (!appended) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(appended.error());
                }
            }
            token = page.value().continuation_token;
            if (token) {
                if (std::find(visited_tokens.begin(), visited_tokens.end(), *token) !=
                    visited_tokens.end()) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(
                        err(base::ErrorCode::kConflict, "file_backup.parent_page_cycle"));
                }
                auto charged =
                    charge_budget(used_bytes, sizeof(std::string) + token->size(), budget_bytes);
                if (!charged) {
                    return base::Result<std::vector<ParentIndexRecord>>::failure(charged.error());
                }
                visited_tokens.push_back(*token);
            }
        } while (token.has_value());
    }
    std::sort(index.begin(), index.end(), path_less);
    for (std::size_t pos = 1; pos < index.size(); ++pos) {
        if (compare_path(index[pos - 1].key, index[pos].key) == 0) {
            return base::Result<std::vector<ParentIndexRecord>>::failure(
                err(base::ErrorCode::kConflict, "file_backup.parent_path_duplicate"));
        }
    }
    return base::Result<std::vector<ParentIndexRecord>>::success(std::move(index));
}

[[nodiscard]] const ParentIndexRecord* find_parent(const std::vector<ParentIndexRecord>& index,
                                                   const PathKey& key) noexcept {
    const auto found =
        std::lower_bound(index.begin(), index.end(), key,
                         [](const ParentIndexRecord& record, const PathKey& candidate) {
                             return compare_path(record.key, candidate) < 0;
                         });
    if (found == index.end() || compare_path(found->key, key) != 0) {
        return nullptr;
    }
    return &*found;
}

[[nodiscard]] const DirectoryPath* find_directory(const std::vector<DirectoryPath>& paths,
                                                  const std::uint64_t entry_id) noexcept {
    const auto found = directory_position(paths, entry_id);
    return found == paths.end() || found->entry_id != entry_id ? nullptr : &*found;
}

[[nodiscard]] base::Result<PathKey> current_path_key(const contracts::FileEntryDesc& entry,
                                                     std::vector<DirectoryPath>& directories,
                                                     std::size_t& used_bytes,
                                                     const std::size_t budget_bytes) {
    if (entry.selection_id.empty()) {
        return base::Result<PathKey>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_backup.current_path_invalid"));
    }
    PathKey key;
    key.selection_id = entry.selection_id;
    key.kind = entry.kind;
    if (entry.parent_entry_id != 0) {
        const auto* parent = find_directory(directories, entry.parent_entry_id);
        if (parent == nullptr || parent->selection_id != entry.selection_id) {
            return base::Result<PathKey>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.current_path_invalid"));
        }
        key.components = parent->components;
    }
    key.components.push_back(entry.name);
    if (entry.kind == contracts::FileEntryKind::kDirectory) {
        const auto bytes = path_bytes(entry.selection_id, key.components);
        auto charged = charge_budget(used_bytes, sizeof(DirectoryPath) + bytes, budget_bytes);
        if (!charged) {
            return base::Result<PathKey>::failure(charged.error());
        }
        DirectoryPath directory;
        directory.entry_id = entry.entry_id;
        directory.selection_id = entry.selection_id;
        directory.components = key.components;
        const auto position = directory_position(directories, directory.entry_id);
        if (position != directories.end() && position->entry_id == directory.entry_id) {
            return base::Result<PathKey>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.current_path_invalid"));
        }
        directories.insert(position, std::move(directory));
    }
    return base::Result<PathKey>::success(std::move(key));
}

[[nodiscard]] base::Result<void>
validate_and_assign_stream_index(contracts::FileEntryDesc& entry,
                                 std::uint32_t& next_stream_index) {
    if (entry.entry_id == 0 || !contracts::is_known_file_entry_kind(entry.kind)) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_object"));
    }
    if (entry.kind == contracts::FileEntryKind::kDirectory) {
        if (!entry.streams.empty() || entry.logical_size != 0) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kInvalidArgument, "directory entry is invalid"));
        }
        return base::Result<void>::success();
    }
    if (entry.streams.size() != 1 ||
        entry.streams.front().stream_kind != contracts::FileStreamKind::kMain ||
        !entry.streams.front().name.bytes.empty() ||
        entry.streams.front().logical_size != entry.logical_size) {
        return base::Result<void>::failure(
            err(base::ErrorCode::kInvalidArgument, "file_source.unsupported_ads"));
    }
    auto& stream = entry.streams.front();
    if (stream.stream_index == 0) {
        stream.stream_index = next_stream_index++;
    } else {
        next_stream_index = (std::max)(next_stream_index, stream.stream_index + 1);
    }
    stream.extents.clear();
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> count_stream(const contracts::FileStreamDesc& stream,
                                              std::uint64_t& stream_count,
                                              std::uint64_t& logical_bytes) {
    auto count = checked_add(stream_count, 1);
    if (!count) {
        return base::Result<void>::failure(count.error());
    }
    auto bytes = checked_add(logical_bytes, stream.logical_size);
    if (!bytes) {
        return base::Result<void>::failure(bytes.error());
    }
    stream_count = count.value();
    logical_bytes = bytes.value();
    return base::Result<void>::success();
}

void mark_local(contracts::FileStreamDesc& stream) {
    stream.content_storage = contracts::FileContentStorage::kLocal;
    stream.parent_stream_index = 0;
}

[[nodiscard]] bool can_reuse_parent(const contracts::FileEntryDesc& current,
                                    const ParentIndexRecord* parent) noexcept {
    return parent != nullptr && parent->main_stream_index != 0 &&
           parent->write_time == current.write_time && parent->logical_size == current.logical_size;
}

[[nodiscard]] base::Result<void> append_local_work(const contracts::FileEntryDesc& entry,
                                                   const std::size_t entry_pos,
                                                   std::vector<FileLocalStreamWork>& work) {
    if (entry.streams.front().logical_size == 0) {
        return base::Result<void>::success();
    }
    FileLocalStreamWork item;
    item.entry_id = entry.entry_id;
    item.stream_index = entry.streams.front().stream_index;
    item.entry_pos = entry_pos;
    item.stream_pos = 0;
    work.push_back(item);
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void> assign_streams(std::vector<contracts::FileEntryDesc>& entries,
                                                const std::vector<ParentIndexRecord>* parent_index,
                                                std::vector<FileLocalStreamWork>& work,
                                                std::uint64_t& stream_count,
                                                std::uint64_t& logical_bytes,
                                                const std::size_t path_budget_bytes,
                                                const base::CancellationToken cancellation) {
    std::uint32_t next_stream_index = 1;
    std::vector<DirectoryPath> current_directories;
    std::size_t current_path_bytes = 0;
    for (std::size_t entry_pos = 0; entry_pos < entries.size(); ++entry_pos) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                err(base::ErrorCode::kCancelled, "file backup plan cancelled"));
        }
        auto& entry = entries[entry_pos];
        auto validated = validate_and_assign_stream_index(entry, next_stream_index);
        if (!validated) {
            return validated;
        }
        auto path =
            current_path_key(entry, current_directories, current_path_bytes, path_budget_bytes);
        if (!path) {
            return base::Result<void>::failure(path.error());
        }
        if (entry.kind == contracts::FileEntryKind::kDirectory) {
            continue;
        }
        auto& stream = entry.streams.front();
        const auto* parent =
            parent_index == nullptr ? nullptr : find_parent(*parent_index, path.value());
        if (can_reuse_parent(entry, parent)) {
            stream.content_storage = contracts::FileContentStorage::kParent;
            stream.parent_stream_index = parent->main_stream_index;
        } else {
            mark_local(stream);
            auto appended = append_local_work(entry, entry_pos, work);
            if (!appended) {
                return appended;
            }
        }
        auto counted = count_stream(stream, stream_count, logical_bytes);
        if (!counted) {
            return counted;
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

    std::vector<ParentIndexRecord> parent_index;
    const std::vector<ParentIndexRecord>* parent_view = nullptr;
    if (request.effective_type == contracts::BackupType::kIncremental) {
        if (request.parent_reader == nullptr) {
            return base::Result<FileSetChangePlanResult>::failure(
                err(base::ErrorCode::kInvalidArgument, "file_backup.parent_required"));
        }
        auto built = build_parent_index(*request.parent_reader, request.parent_index_budget_bytes,
                                        cancellation);
        if (!built) {
            return base::Result<FileSetChangePlanResult>::failure(built.error());
        }
        parent_index = std::move(built).value();
        parent_view = &parent_index;
    } else if (request.effective_type != contracts::BackupType::kFull) {
        return base::Result<FileSetChangePlanResult>::failure(
            err(base::ErrorCode::kInvalidArgument, "file backup type is unsupported"));
    }

    auto assigned =
        assign_streams(result.entries, parent_view, result.local_streams, result.stream_count,
                       result.logical_bytes, request.parent_index_budget_bytes, cancellation);
    if (!assigned) {
        return base::Result<FileSetChangePlanResult>::failure(assigned.error());
    }
    return base::Result<FileSetChangePlanResult>::success(std::move(result));
}

} // namespace aegra::pipeline
