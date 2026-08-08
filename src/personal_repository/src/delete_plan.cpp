#include "aegra/personal_repository/delete_plan.h"

#include "json_codec.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <functional>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::personal_repository {
namespace {

using detail::Json;

[[nodiscard]] base::Error conflict(std::string message) {
    return {base::ErrorCode::kConflict, std::move(message)};
}

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

[[nodiscard]] base::Error not_found() {
    return {base::ErrorCode::kNotFound, "recovery point does not exist"};
}

[[nodiscard]] std::string part_suffix(const std::uint32_t part_index) {
    std::array<char, 4> buffer{'.', '0', '0', '0'};
    buffer[1] = static_cast<char>('0' + ((part_index / 100) % 10));
    buffer[2] = static_cast<char>('0' + ((part_index / 10) % 10));
    buffer[3] = static_cast<char>('0' + (part_index % 10));
    return std::string(buffer.data(), buffer.size());
}

[[nodiscard]] base::Result<std::vector<CatalogEntry>>
collect_subtree(const std::vector<CatalogEntry>& all_entries, const std::string_view root_uuid) {
    std::map<std::string, CatalogEntry, std::less<>> by_id;
    std::map<std::string, std::vector<std::string>, std::less<>> children;
    for (const auto& entry : all_entries) {
        by_id.emplace(entry.file_uuid, entry);
        if (entry.parent_uuid) {
            children[*entry.parent_uuid].push_back(entry.file_uuid);
        }
    }
    if (!by_id.contains(std::string(root_uuid))) {
        return base::Result<std::vector<CatalogEntry>>::failure(not_found());
    }
    std::vector<std::string> stack{std::string(root_uuid)};
    std::set<std::string, std::less<>> selected;
    while (!stack.empty()) {
        auto current = std::move(stack.back());
        stack.pop_back();
        if (!selected.insert(current).second) {
            return base::Result<std::vector<CatalogEntry>>::failure(
                conflict("delete plan graph contains a cycle"));
        }
        const auto found = children.find(current);
        if (found != children.end()) {
            for (const auto& child : found->second) {
                stack.push_back(child);
            }
        }
    }
    std::vector<CatalogEntry> ordered;
    ordered.reserve(selected.size());
    std::function<void(const std::string&)> visit = [&](const std::string& uuid) {
        const auto found = children.find(uuid);
        if (found != children.end()) {
            for (const auto& child : found->second) {
                if (selected.contains(child)) {
                    visit(child);
                }
            }
        }
        ordered.push_back(by_id.at(uuid));
    };
    visit(std::string(root_uuid));
    return base::Result<std::vector<CatalogEntry>>::success(std::move(ordered));
}

[[nodiscard]] base::Result<void> write_all(ports::IStagedObjectWriteSession& session,
                                           const std::string_view body,
                                           const base::CancellationToken cancellation) {
    const auto bytes = std::as_bytes(std::span(body.data(), body.size()));
    return session.write(bytes, cancellation);
}

[[nodiscard]] base::Result<std::string>
read_entire_object(ports::IObjectReader& reader, const std::string& key,
                   const base::CancellationToken cancellation) {
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes) {
        return base::Result<std::string>::failure(attributes.error());
    }
    if (attributes.value().size_bytes == 0 ||
        attributes.value().size_bytes > 4ULL * 1024ULL * 1024ULL ||
        attributes.value().size_bytes >
            static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kCorruptData, "repository object size is invalid"});
    }
    std::string body(static_cast<std::size_t>(attributes.value().size_bytes), '\0');
    std::size_t offset = 0;
    while (offset < body.size()) {
        auto read = reader.read_range(
            key, offset,
            std::as_writable_bytes(std::span(body.data() + offset, body.size() - offset)),
            cancellation);
        if (!read) {
            return base::Result<std::string>::failure(read.error());
        }
        if (read.value() == 0) {
            return base::Result<std::string>::failure(
                {base::ErrorCode::kCorruptData, "repository object is truncated"});
        }
        offset += read.value();
    }
    return base::Result<std::string>::success(std::move(body));
}

[[nodiscard]] base::Result<void> publish_tombstone(ports::IStagedObjectWriter& writer,
                                                   ports::IObjectPublisher& publisher,
                                                   const DeletePlan& plan,
                                                   const base::CancellationToken cancellation) {
    auto encoded = encode_deletion_tombstone_json(plan.tombstone);
    if (!encoded) {
        return base::Result<void>::failure(encoded.error());
    }
    const auto key = deletion_tombstone_key(plan.tombstone.operation_uuid);
    const auto staging_key = "staging/" + plan.tombstone.operation_uuid + "/deletion.tombstone";
    auto session = writer.begin_staged_write(staging_key, cancellation);
    if (!session) {
        return base::Result<void>::failure(session.error());
    }
    auto written = write_all(*session.value(), encoded.value(), cancellation);
    if (!written) {
        session.value()->abort();
        return written;
    }
    auto completed = session.value()->complete(cancellation);
    if (!completed) {
        session.value()->abort();
        return completed;
    }
    ports::ObjectPublishRequest publish_request;
    publish_request.staging_key = staging_key;
    publish_request.destination_key = key;
    publish_request.condition = ports::PublishCondition::kCreateOnly;
    auto published = publisher.publish(publish_request, cancellation);
    if (!published) {
        return base::Result<void>::failure(published.error());
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<bool> delete_targets_absent(ports::IObjectReader& reader,
                                                       const DeletePlan& plan,
                                                       const base::CancellationToken cancellation) {
    for (const auto& target : plan.tombstone.targets) {
        for (const auto& member : target.members) {
            auto attributes = reader.get_attributes(member.key, cancellation);
            if (attributes) {
                return base::Result<bool>::success(false);
            }
            if (attributes.error().code != base::ErrorCode::kNotFound) {
                return base::Result<bool>::failure(attributes.error());
            }
        }
        auto catalog = reader.get_attributes(catalog_entry_key(target.file_uuid), cancellation);
        if (catalog) {
            return base::Result<bool>::success(false);
        }
        if (catalog.error().code != base::ErrorCode::kNotFound) {
            return base::Result<bool>::failure(catalog.error());
        }
    }
    return base::Result<bool>::success(true);
}

[[nodiscard]] base::Result<void> ensure_tombstone(ports::IObjectReader& reader,
                                                  ports::IStagedObjectWriter& writer,
                                                  ports::IObjectPublisher& publisher,
                                                  const DeletePlan& plan,
                                                  const base::CancellationToken cancellation) {
    auto existing = load_published_tombstone(reader, plan.tombstone.operation_uuid, cancellation);
    if (!existing) {
        return base::Result<void>::failure(existing.error());
    }
    if (existing.value()) {
        return *existing.value() == plan.tombstone
                   ? base::Result<void>::success()
                   : base::Result<void>::failure(
                         conflict("delete tombstone operation UUID content mismatch"));
    }

    auto targets_absent = delete_targets_absent(reader, plan, cancellation);
    if (!targets_absent) {
        return base::Result<void>::failure(targets_absent.error());
    }
    if (targets_absent.value()) {
        return base::Result<void>::success();
    }

    auto published = publish_tombstone(writer, publisher, plan, cancellation);
    if (published) {
        return published;
    }
    if (published.error().code != base::ErrorCode::kConflict) {
        return published;
    }
    existing = load_published_tombstone(reader, plan.tombstone.operation_uuid, cancellation);
    if (!existing) {
        return base::Result<void>::failure(existing.error());
    }
    if (!existing.value()) {
        return base::Result<void>::failure(
            conflict("delete tombstone create conflict without readable tombstone"));
    }
    if (*existing.value() != plan.tombstone) {
        return base::Result<void>::failure(
            conflict("delete tombstone operation UUID content mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
delete_key_idempotent(ports::IObjectReader& reader, ports::IObjectDeleter& deleter,
                      const std::string& key, const std::string& operation_id,
                      std::optional<std::string> expected_generation,
                      const base::CancellationToken cancellation) {
    ports::ObjectDeleteRequest request;
    request.key = key;
    request.operation_id = operation_id;
    request.expected_generation = std::move(expected_generation);
    auto deleted = deleter.delete_object(request, cancellation);
    if (deleted) {
        return deleted;
    }
    if (deleted.error().code == base::ErrorCode::kOutcomeUnknown) {
        auto attributes = reader.get_attributes(key, cancellation);
        if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
            return base::Result<void>::success();
        }
        if (!attributes) {
            return base::Result<void>::failure(attributes.error());
        }
        return base::Result<void>::failure(deleted.error());
    }
    if (deleted.error().code == base::ErrorCode::kNotFound) {
        return base::Result<void>::success();
    }
    return deleted;
}

[[nodiscard]] base::Result<void> delete_catalog_entry(ports::IObjectReader& reader,
                                                      ports::IObjectDeleter& deleter,
                                                      const DeletionTarget& target,
                                                      const std::string& operation_id,
                                                      const base::CancellationToken cancellation) {
    const auto key = catalog_entry_key(target.file_uuid);
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
        return base::Result<void>::success();
    }
    if (!attributes) {
        return base::Result<void>::failure(attributes.error());
    }
    auto body = read_entire_object(reader, key, cancellation);
    if (!body) {
        return base::Result<void>::failure(body.error());
    }
    auto entry = decode_catalog_entry_json(body.value());
    if (!entry) {
        return base::Result<void>::failure(entry.error());
    }
    if (entry.value().file_uuid != target.file_uuid ||
        entry.value().catalog_generation != target.catalog_generation ||
        entry.value().archive_main_key != target.archive_main_key) {
        return base::Result<void>::failure(conflict("delete catalog entry identity changed"));
    }
    return delete_key_idempotent(reader, deleter, key, operation_id, attributes.value().generation,
                                 cancellation);
}

} // namespace

base::Result<std::vector<std::string>> build_archive_member_keys(const CatalogEntry& entry) {
    auto valid = validate_catalog_entry(entry);
    if (!valid) {
        return base::Result<std::vector<std::string>>::failure(valid.error());
    }
    std::vector<std::string> members;
    members.reserve(entry.split_part_count + (entry.has_sidecar ? 1U : 0U));
    if (entry.has_sidecar) {
        members.push_back(entry.archive_main_key + ".bhx");
    }
    for (std::uint32_t part = entry.split_part_count; part >= 2; --part) {
        members.push_back(entry.archive_main_key + part_suffix(part - 1));
    }
    members.push_back(entry.archive_main_key);
    return base::Result<std::vector<std::string>>::success(std::move(members));
}

base::Result<DeletePlan> plan_delete_recovery_points(
    const std::vector<CatalogEntry>& entries, const std::string_view root_file_uuid,
    const std::string_view operation_uuid, const std::uint64_t created_utc_ms,
    const std::uint64_t expires_utc_ms, const std::string_view repository_connection_id,
    const ArchiveMemberGenerationResolver& resolve_member_generation) {
    if (operation_uuid.empty() || created_utc_ms == 0 || entries.empty() || expires_utc_ms == 0 ||
        repository_connection_id.empty() || !resolve_member_generation) {
        return base::Result<DeletePlan>::failure(invalid("delete plan request is invalid"));
    }
    auto graph = RecoveryPointGraph::build(entries);
    if (!graph) {
        return base::Result<DeletePlan>::failure(graph.error());
    }
    auto subtree = collect_subtree(entries, root_file_uuid);
    if (!subtree) {
        return base::Result<DeletePlan>::failure(subtree.error());
    }
    DeletePlan plan;
    plan.expires_utc_ms = expires_utc_ms;
    plan.repository_connection_id = std::string(repository_connection_id);
    plan.tombstone.repository_uuid = entries.front().repository_uuid;
    plan.tombstone.operation_uuid = std::string(operation_uuid);
    plan.tombstone.created_utc_ms = created_utc_ms;
    plan.tombstone.targets.reserve(subtree.value().size());
    for (const auto& entry : subtree.value()) {
        auto members = build_archive_member_keys(entry);
        if (!members) {
            return base::Result<DeletePlan>::failure(members.error());
        }
        DeletionTarget target;
        target.file_uuid = entry.file_uuid;
        target.catalog_generation = entry.catalog_generation;
        target.content_kind = entry.content_kind;
        target.archive_main_key = entry.archive_main_key;
        target.members.reserve(members.value().size());
        for (auto& key : members.value()) {
            auto generation = resolve_member_generation(key);
            if (!generation) {
                return base::Result<DeletePlan>::failure(generation.error());
            }
            target.members.push_back({std::move(key), std::move(generation).value()});
        }
        plan.tombstone.targets.push_back(std::move(target));
    }
    auto valid = validate_deletion_tombstone(plan.tombstone);
    if (!valid) {
        return base::Result<DeletePlan>::failure(valid.error());
    }
    return base::Result<DeletePlan>::success(std::move(plan));
}

base::Result<void> revalidate_delete_plan(const DeletePlan& plan,
                                          const RecoveryPointGraph& current_graph) {
    auto valid = validate_deletion_tombstone(plan.tombstone);
    if (!valid) {
        return valid;
    }
    for (const auto& target : plan.tombstone.targets) {
        const auto* entry = current_graph.find(target.file_uuid);
        if (entry == nullptr) {
            continue;
        }
        if (entry->catalog_generation != target.catalog_generation ||
            entry->archive_main_key != target.archive_main_key) {
            return base::Result<void>::failure(
                conflict("delete plan generation or archive key changed"));
        }
    }
    return base::Result<void>::success();
}

base::Result<void> revalidate_delete_plan_strict(const DeletePlan& plan,
                                                 const std::vector<CatalogEntry>& current_entries) {
    if (plan.tombstone.targets.empty()) {
        return base::Result<void>::failure(invalid("delete plan is empty"));
    }
    const auto& root_uuid = plan.tombstone.targets.back().file_uuid;
    auto expected = collect_subtree(current_entries, root_uuid);
    if (!expected) {
        return base::Result<void>::failure(expected.error());
    }
    if (expected.value().size() != plan.tombstone.targets.size()) {
        return base::Result<void>::failure(conflict("delete plan descendant set changed"));
    }
    for (std::size_t index = 0; index < expected.value().size(); ++index) {
        const auto& entry = expected.value()[index];
        const auto& target = plan.tombstone.targets[index];
        auto member_keys = build_archive_member_keys(entry);
        if (!member_keys) {
            return base::Result<void>::failure(member_keys.error());
        }
        if (entry.file_uuid != target.file_uuid ||
            entry.catalog_generation != target.catalog_generation ||
            entry.archive_main_key != target.archive_main_key ||
            member_keys.value().size() != target.members.size() ||
            !std::ranges::equal(member_keys.value(), target.members, {}, {},
                                &DeletionMember::key)) {
            return base::Result<void>::failure(conflict("delete plan target mismatch"));
        }
    }
    return base::Result<void>::success();
}

base::Result<void> revalidate_delete_plan_members(const DeletePlan& plan,
                                                  ports::IObjectReader& reader,
                                                  const base::CancellationToken cancellation) {
    auto valid = validate_deletion_tombstone(plan.tombstone);
    if (!valid) {
        return valid;
    }
    for (const auto& target : plan.tombstone.targets) {
        for (const auto& member : target.members) {
            auto attributes = reader.get_attributes(member.key, cancellation);
            if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
                continue;
            }
            if (!attributes) {
                return base::Result<void>::failure(attributes.error());
            }
            if (!member.generation || attributes.value().generation != *member.generation) {
                return base::Result<void>::failure(
                    conflict("delete plan archive member generation changed"));
            }
        }
    }
    return base::Result<void>::success();
}

base::Result<std::string> encode_delete_plan_document(const DeletePlan& plan) {
    auto tombstone = encode_deletion_tombstone_json(plan.tombstone);
    if (!tombstone) {
        return base::Result<std::string>::failure(tombstone.error());
    }
    auto parsed = detail::parse_json_object(tombstone.value(), {});
    if (!parsed) {
        return base::Result<std::string>::failure(parsed.error());
    }
    Json root{{"schema_version", 1},
              {"kind", "aegra_personal_delete_plan"},
              {"expires_utc_ms", plan.expires_utc_ms},
              {"repository_connection_id", plan.repository_connection_id},
              {"tombstone", std::move(parsed).value()}};
    return base::Result<std::string>::success(root.dump());
}

base::Result<DeletePlan> decode_delete_plan_document(const std::string_view encoded,
                                                     const CatalogCodecLimits& limits) {
    auto root = detail::parse_json_object(encoded, limits);
    if (!root) {
        return base::Result<DeletePlan>::failure(root.error());
    }
    constexpr std::array<std::string_view, 5> keys{"schema_version", "kind", "expires_utc_ms",
                                                   "repository_connection_id", "tombstone"};
    auto keys_ok = detail::require_exact_keys(root.value(), keys);
    if (!keys_ok) {
        return base::Result<DeletePlan>::failure(keys_ok.error());
    }
    auto schema = detail::get_unsigned<std::uint32_t>(root.value(), "schema_version");
    auto expires = detail::get_unsigned<std::uint64_t>(root.value(), "expires_utc_ms");
    if (!schema || schema.value() != 1 || !expires || expires.value() == 0) {
        return base::Result<DeletePlan>::failure(invalid("delete plan document is invalid"));
    }
    try {
        if (root.value().at("kind").get<std::string>() != "aegra_personal_delete_plan") {
            return base::Result<DeletePlan>::failure(invalid("delete plan kind is invalid"));
        }
        DeletePlan plan;
        plan.expires_utc_ms = expires.value();
        plan.repository_connection_id =
            root.value().at("repository_connection_id").get<std::string>();
        auto tombstone_json = root.value().at("tombstone").dump();
        auto tombstone = decode_deletion_tombstone_json(tombstone_json, limits);
        if (!tombstone) {
            return base::Result<DeletePlan>::failure(tombstone.error());
        }
        plan.tombstone = std::move(tombstone).value();
        if (plan.repository_connection_id.empty()) {
            return base::Result<DeletePlan>::failure(invalid("delete plan connection is invalid"));
        }
        return base::Result<DeletePlan>::success(std::move(plan));
    } catch (const Json::exception&) {
        return base::Result<DeletePlan>::failure(
            invalid("delete plan document field type is invalid"));
    }
}

base::Result<std::optional<DeletionTombstone>>
load_published_tombstone(ports::IObjectReader& reader, const std::string_view operation_uuid,
                         const base::CancellationToken cancellation) {
    const auto key = deletion_tombstone_key(operation_uuid);
    auto attributes = reader.get_attributes(key, cancellation);
    if (!attributes) {
        if (attributes.error().code == base::ErrorCode::kNotFound) {
            return base::Result<std::optional<DeletionTombstone>>::success(std::nullopt);
        }
        return base::Result<std::optional<DeletionTombstone>>::failure(attributes.error());
    }
    auto body = read_entire_object(reader, key, cancellation);
    if (!body) {
        return base::Result<std::optional<DeletionTombstone>>::failure(body.error());
    }
    auto tombstone = decode_deletion_tombstone_json(body.value());
    if (!tombstone) {
        return base::Result<std::optional<DeletionTombstone>>::failure(tombstone.error());
    }
    return base::Result<std::optional<DeletionTombstone>>::success(std::move(tombstone).value());
}

DeletePlanExecutor::DeletePlanExecutor(ports::IObjectReader& reader,
                                       ports::IStagedObjectWriter& writer,
                                       ports::IObjectPublisher& publisher,
                                       ports::IObjectDeleter& deleter) noexcept
    : reader_(reader), writer_(writer), publisher_(publisher), deleter_(deleter) {}

base::Result<void> DeletePlanExecutor::execute(const DeletePlan& plan,
                                               const base::CancellationToken cancellation) const {
    auto valid = validate_deletion_tombstone(plan.tombstone);
    if (!valid) {
        return valid;
    }
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCancelled, "delete plan execute cancelled"});
    }
    auto ensured = ensure_tombstone(reader_, writer_, publisher_, plan, cancellation);
    if (!ensured) {
        return ensured;
    }
    for (const auto& target : plan.tombstone.targets) {
        for (const auto& member : target.members) {
            if (cancellation.stop_requested()) {
                return base::Result<void>::failure(
                    base::Error{base::ErrorCode::kCancelled, "delete plan execute cancelled"});
            }
            if (!member.generation) {
                auto attributes = reader_.get_attributes(member.key, cancellation);
                if (!attributes && attributes.error().code == base::ErrorCode::kNotFound) {
                    continue;
                }
                if (!attributes) {
                    return base::Result<void>::failure(attributes.error());
                }
                return base::Result<void>::failure(
                    conflict("archive member appeared after delete planning"));
            }
            auto deleted =
                delete_key_idempotent(reader_, deleter_, member.key, plan.tombstone.operation_uuid,
                                      member.generation, cancellation);
            if (!deleted) {
                return deleted;
            }
        }
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                base::Error{base::ErrorCode::kCancelled, "delete plan execute cancelled"});
        }
        auto catalog_deleted = delete_catalog_entry(reader_, deleter_, target,
                                                    plan.tombstone.operation_uuid, cancellation);
        if (!catalog_deleted) {
            return catalog_deleted;
        }
    }
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCancelled, "delete plan execute cancelled"});
    }
    return delete_key_idempotent(reader_, deleter_,
                                 deletion_tombstone_key(plan.tombstone.operation_uuid),
                                 plan.tombstone.operation_uuid, std::nullopt, cancellation);
}

std::string deletion_tombstone_key(const std::string_view operation_uuid) {
    return "catalog/deletions/" + std::string(operation_uuid) + ".tombstone";
}

std::string catalog_entry_key(const std::string_view file_uuid) {
    return "catalog/recovery-points/" + std::string(file_uuid) + ".entry";
}

} // namespace aegra::personal_repository
