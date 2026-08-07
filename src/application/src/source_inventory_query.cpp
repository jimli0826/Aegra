#include "aegra/application/source_inventory_query.h"

#include "application_ids.h"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <utility>

namespace aegra::application {
namespace {

[[nodiscard]] base::Result<std::optional<std::string>>
decode_inventory_token(const std::optional<std::string>& token, const bool include_unavailable) {
    if (!token) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    const auto expected_prefix =
        include_unavailable ? std::string_view{"inv|1|"} : std::string_view{"inv|0|"};
    if (token->size() <= expected_prefix.size() ||
        token->compare(0, expected_prefix.size(), expected_prefix) != 0) {
        return base::Result<std::optional<std::string>>::failure(
            {base::ErrorCode::kInvalidArgument, "inventory continuation token is invalid"});
    }
    auto source_id = token->substr(expected_prefix.size());
    if (source_id.empty()) {
        return base::Result<std::optional<std::string>>::failure(
            {base::ErrorCode::kInvalidArgument, "inventory continuation token is invalid"});
    }
    return base::Result<std::optional<std::string>>::success(std::move(source_id));
}

[[nodiscard]] std::string encode_inventory_token(const bool include_unavailable,
                                                 const std::string_view source_id) {
    return std::string(include_unavailable ? "inv|1|" : "inv|0|") + std::string(source_id);
}

/// Wire field is a short drive letter (max 16). Folder mounts / long paths are dropped.
[[nodiscard]] std::string normalize_mount_letter(const std::string& raw) {
    if (raw.empty()) {
        return {};
    }
    const auto letter = static_cast<unsigned char>(raw.front());
    if (!((letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z'))) {
        return {};
    }
    if (raw.size() == 1) {
        return std::string(1, static_cast<char>(std::toupper(letter))) + ":";
    }
    if (raw[1] != ':') {
        return {};
    }
    // Accept "C:", "C:\", or "C:/" only — not folder mounts like "C:\data".
    if (raw.size() > 3) {
        return {};
    }
    if (raw.size() == 3 && raw[2] != '\\' && raw[2] != '/') {
        return {};
    }
    return std::string(1, static_cast<char>(std::toupper(letter))) + ":";
}

[[nodiscard]] contracts::SourceInventoryItem map_item(const ports::SourceInventoryRecord& record) {
    contracts::SourceInventoryItem item;
    item.source_id = record.source_id;
    item.display_name = record.display_name;
    item.kind = record.kind;
    item.availability = record.availability;
    item.capacity_bytes = record.capacity_bytes;
    item.free_bytes = record.free_bytes > record.capacity_bytes ? record.capacity_bytes
                                                                : record.free_bytes;
    item.disk_capacity_bytes = record.disk_capacity_bytes;
    item.is_system = record.is_system;
    item.is_read_only = record.is_read_only;
    item.is_selectable = detail::is_source_selectable(record);
    item.disk_number = record.disk_number;
    item.mount_letter = normalize_mount_letter(record.mount_letter);
    item.volume_label = record.volume_label;
    item.health_status = record.health_status.empty() ? "Healthy" : record.health_status;
    item.partition_style = record.partition_style.empty() ? "GPT" : record.partition_style;
    item.media_type = record.media_type.empty() ? "Unknown" : record.media_type;
    // Contracts reject empty display names / oversized text — clamp for wire safety.
    if (item.display_name.empty()) {
        item.display_name = item.source_id;
    }
    if (item.display_name.size() > 256) {
        item.display_name.resize(256);
    }
    if (item.volume_label.size() > 256) {
        item.volume_label.resize(256);
    }
    if (item.health_status.size() > 256) {
        item.health_status.resize(256);
    }
    return item;
}

} // namespace

SourceInventoryQuery::SourceInventoryQuery(ports::ISourceInventory& inventory) noexcept
    : inventory_(inventory) {}

base::Result<contracts::SourceInventoryPage>
SourceInventoryQuery::list_sources(const contracts::SourceInventoryListRequest& request,
                                   const base::CancellationToken cancellation) {
    auto valid = contracts::validate_source_inventory_list_request(request);
    if (!valid) {
        return base::Result<contracts::SourceInventoryPage>::failure(valid.error());
    }
    auto token =
        decode_inventory_token(request.page.continuation_token, request.include_unavailable);
    if (!token) {
        return base::Result<contracts::SourceInventoryPage>::failure(token.error());
    }
    auto sources = inventory_.list_sources(cancellation);
    if (!sources) {
        return base::Result<contracts::SourceInventoryPage>::failure(sources.error());
    }
    std::vector<contracts::SourceInventoryItem> mapped;
    mapped.reserve(sources.value().size());
    for (const auto& source : sources.value()) {
        if (!request.include_unavailable &&
            source.availability != contracts::SourceAvailability::kAvailable) {
            continue;
        }
        if (source.source_id.empty() || source.stable_key.empty()) {
            // Skip unusable identities; do not fail the whole inventory page.
            continue;
        }
        auto item = map_item(source);
        if (!contracts::validate_source_inventory_item(item)) {
            // One odd volume (long mount path, bad label, etc.) must not block Backup UI.
            continue;
        }
        mapped.push_back(std::move(item));
    }
    std::ranges::sort(mapped, [](const contracts::SourceInventoryItem& left,
                                 const contracts::SourceInventoryItem& right) {
        return left.source_id < right.source_id;
    });
    if (std::ranges::adjacent_find(mapped, [](const auto& left, const auto& right) {
            return left.source_id == right.source_id;
        }) != mapped.end()) {
        return base::Result<contracts::SourceInventoryPage>::failure(
            {base::ErrorCode::kConflict, "inventory source ids are not unique"});
    }

    contracts::SourceInventoryPage page;
    for (const auto& item : mapped) {
        if (token.value() && item.source_id <= *token.value()) {
            continue;
        }
        if (page.items.size() >= request.page.maximum_results) {
            page.continuation_token =
                encode_inventory_token(request.include_unavailable, page.items.back().source_id);
            break;
        }
        page.items.push_back(item);
    }
    auto valid_page = contracts::validate_source_inventory_page(page);
    return valid_page ? base::Result<contracts::SourceInventoryPage>::success(std::move(page))
                      : base::Result<contracts::SourceInventoryPage>::failure(valid_page.error());
}

base::Result<ports::SourceInventoryRecord>
SourceInventoryQuery::resolve_source(const std::string_view source_id,
                                     const base::CancellationToken cancellation) {
    if (source_id.empty()) {
        return base::Result<ports::SourceInventoryRecord>::failure(
            {base::ErrorCode::kInvalidArgument, "source id is empty"});
    }
    auto sources = inventory_.list_sources(cancellation);
    if (!sources) {
        return base::Result<ports::SourceInventoryRecord>::failure(sources.error());
    }
    const auto found = std::ranges::find_if(
        sources.value(), [&](const auto& source) { return source.source_id == source_id; });
    if (found == sources.value().end()) {
        return base::Result<ports::SourceInventoryRecord>::failure(
            {base::ErrorCode::kNotFound, "source id not found"});
    }
    if (!detail::is_source_selectable(*found) || found->stable_key.empty()) {
        return base::Result<ports::SourceInventoryRecord>::failure(
            {base::ErrorCode::kConflict, "source is not selectable"});
    }
    return base::Result<ports::SourceInventoryRecord>::success(*found);
}

} // namespace aegra::application
