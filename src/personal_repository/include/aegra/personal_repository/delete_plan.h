#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/chain_graph.h"
#include "aegra/ports/object_storage.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::personal_repository {

using ArchiveMemberGenerationResolver =
    std::function<base::Result<std::optional<std::string>>(std::string_view)>;

// Immutable delete plan body (tombstone) plus control-plane execution metadata.
struct DeletePlan final {
    DeletionTombstone tombstone;
    std::uint64_t expires_utc_ms{0};
    std::string repository_connection_id;

    [[nodiscard]] bool operator==(const DeletePlan&) const = default;
};

[[nodiscard]] base::Result<std::vector<std::string>>
build_archive_member_keys(const CatalogEntry& entry);

[[nodiscard]] base::Result<DeletePlan>
plan_delete_recovery_points(const std::vector<CatalogEntry>& entries,
                            std::string_view root_file_uuid, std::string_view operation_uuid,
                            std::uint64_t created_utc_ms, std::uint64_t expires_utc_ms,
                            std::string_view repository_connection_id,
                            const ArchiveMemberGenerationResolver& resolve_member_generation);

// First-execute check: planned targets must still exist with matching generation order.
[[nodiscard]] base::Result<void>
revalidate_delete_plan_strict(const DeletePlan& plan,
                              const std::vector<CatalogEntry>& current_entries);

// Missing members remain idempotent; existing members must retain the planned generation.
[[nodiscard]] base::Result<void>
revalidate_delete_plan_members(const DeletePlan& plan, ports::IObjectReader& reader,
                               base::CancellationToken cancellation);

[[nodiscard]] base::Result<void> revalidate_delete_plan(const DeletePlan& plan,
                                                        const RecoveryPointGraph& current_graph);

// Durable plan document (JSON) for staging/delete-plans/<token>.json
[[nodiscard]] base::Result<std::string> encode_delete_plan_document(const DeletePlan& plan);
[[nodiscard]] base::Result<DeletePlan>
decode_delete_plan_document(std::string_view encoded, const CatalogCodecLimits& limits = {});

// Load published tombstone from catalog/deletions/<op>.tombstone when present.
[[nodiscard]] base::Result<std::optional<DeletionTombstone>>
load_published_tombstone(ports::IObjectReader& reader, std::string_view operation_uuid,
                         base::CancellationToken cancellation);

// Executes using tombstone authority after first publish. Partial retries continue safely.
// Create-only tombstone conflict requires byte-identical existing tombstone.
class DeletePlanExecutor final {
  public:
    DeletePlanExecutor(ports::IObjectReader& reader, ports::IStagedObjectWriter& writer,
                       ports::IObjectPublisher& publisher, ports::IObjectDeleter& deleter) noexcept;

    [[nodiscard]] base::Result<void> execute(const DeletePlan& plan,
                                             base::CancellationToken cancellation) const;

  private:
    ports::IObjectReader& reader_;
    ports::IStagedObjectWriter& writer_;
    ports::IObjectPublisher& publisher_;
    ports::IObjectDeleter& deleter_;
};

[[nodiscard]] std::string deletion_tombstone_key(std::string_view operation_uuid);
[[nodiscard]] std::string catalog_entry_key(std::string_view file_uuid);

} // namespace aegra::personal_repository
