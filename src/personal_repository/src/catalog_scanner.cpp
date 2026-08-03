#include "aegra/personal_repository/catalog_scanner.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::personal_repository {
namespace {

[[nodiscard]] base::Error scanner_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] base::Result<void> consume_budget(const std::uint64_t bytes,
                                                std::uint64_t& remaining) {
    if (bytes > remaining) {
        return base::Result<void>::failure(
            scanner_error(base::ErrorCode::kCorruptData, "repository scan byte limit exceeded"));
    }
    remaining -= bytes;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::string> read_object(ports::IObjectReader& reader,
                                                    const ports::ObjectAttributes& listed,
                                                    const CatalogCodecLimits& codec,
                                                    std::uint64_t& remaining,
                                                    const base::CancellationToken cancellation) {
    if (listed.size_bytes > codec.maximum_document_bytes ||
        listed.size_bytes > static_cast<std::uint64_t>((std::numeric_limits<std::size_t>::max)())) {
        return base::Result<std::string>::failure(
            scanner_error(base::ErrorCode::kCorruptData, "repository object is too large"));
    }
    auto budget = consume_budget(listed.size_bytes, remaining);
    if (!budget) {
        return base::Result<std::string>::failure(budget.error());
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(listed.size_bytes));
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        auto read =
            reader.read_range(listed.key, offset, std::span(bytes).subspan(offset), cancellation);
        if (!read) {
            return base::Result<std::string>::failure(read.error());
        }
        if (read.value() == 0 || read.value() > bytes.size() - offset) {
            return base::Result<std::string>::failure(
                scanner_error(base::ErrorCode::kCorruptData, "repository object was truncated"));
        }
        offset += read.value();
    }
    auto current = reader.get_attributes(listed.key, cancellation);
    if (!current || current.value() != listed) {
        return base::Result<std::string>::failure(
            current
                ? scanner_error(base::ErrorCode::kConflict, "repository object changed during scan")
                : current.error());
    }
    return base::Result<std::string>::success(
        std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

[[nodiscard]] base::Result<std::vector<ports::ObjectAttributes>>
enumerate_all(ports::IPrefixEnumerator& enumerator, const std::string& prefix,
              const std::uint32_t maximum_objects, const base::CancellationToken cancellation) {
    std::vector<ports::ObjectAttributes> objects;
    std::optional<std::string> token;
    do {
        ports::ObjectListRequest request{prefix + "/", token, 1'000};
        auto page = enumerator.enumerate(request, cancellation);
        if (!page) {
            return base::Result<std::vector<ports::ObjectAttributes>>::failure(page.error());
        }
        if (objects.size() + page.value().objects.size() > maximum_objects) {
            return base::Result<std::vector<ports::ObjectAttributes>>::failure(scanner_error(
                base::ErrorCode::kCorruptData, "repository object count limit exceeded"));
        }
        for (auto& object : page.value().objects) {
            if (!object.key.starts_with(request.prefix) ||
                (!objects.empty() && object.key <= objects.back().key)) {
                return base::Result<std::vector<ports::ObjectAttributes>>::failure(scanner_error(
                    base::ErrorCode::kConflict, "repository enumeration order is invalid"));
            }
            objects.push_back(std::move(object));
        }
        const auto next = page.value().continuation_token;
        if (next &&
            (page.value().objects.empty() || next == token || *next != objects.back().key)) {
            return base::Result<std::vector<ports::ObjectAttributes>>::failure(scanner_error(
                base::ErrorCode::kConflict, "repository continuation token is invalid"));
        }
        token = next;
    } while (token);
    return base::Result<std::vector<ports::ObjectAttributes>>::success(std::move(objects));
}

[[nodiscard]] std::string catalog_key(const RepositoryDescriptor& descriptor,
                                      const std::string_view file_uuid) {
    return descriptor.catalog_prefix + "/" + std::string(file_uuid) + ".entry";
}

[[nodiscard]] std::string tombstone_key(const RepositoryDescriptor& descriptor,
                                        const std::string_view operation_uuid) {
    return descriptor.deletion_prefix + "/" + std::string(operation_uuid) + ".tombstone";
}

struct LoadedCatalog final {
    RepositoryDescriptor descriptor;
    std::vector<CatalogEntry> entries;
};

[[nodiscard]] base::Result<RepositoryDescriptor>
load_descriptor(ports::IObjectReader& reader, const CatalogScannerLimits& limits,
                std::uint64_t& remaining, const base::CancellationToken cancellation) {
    auto attributes = reader.get_attributes("aegra.repository", cancellation);
    if (!attributes) {
        return base::Result<RepositoryDescriptor>::failure(attributes.error());
    }
    auto encoded = read_object(reader, attributes.value(), limits.codec, remaining, cancellation);
    return encoded ? decode_repository_descriptor_json(encoded.value(), limits.codec)
                   : base::Result<RepositoryDescriptor>::failure(encoded.error());
}

[[nodiscard]] base::Result<std::set<std::string, std::less<>>>
load_hidden(ports::IObjectReader& reader, ports::IPrefixEnumerator& enumerator,
            const RepositoryDescriptor& descriptor, const CatalogScannerLimits& limits,
            std::uint64_t& remaining, const base::CancellationToken cancellation) {
    auto objects = enumerate_all(enumerator, descriptor.deletion_prefix,
                                 limits.maximum_tombstone_objects, cancellation);
    if (!objects) {
        return base::Result<std::set<std::string, std::less<>>>::failure(objects.error());
    }
    std::set<std::string, std::less<>> hidden;
    for (const auto& object : objects.value()) {
        auto encoded = read_object(reader, object, limits.codec, remaining, cancellation);
        auto tombstone = encoded ? decode_deletion_tombstone_json(encoded.value(), limits.codec)
                                 : base::Result<DeletionTombstone>::failure(encoded.error());
        if (!tombstone || tombstone.value().repository_uuid != descriptor.repository_uuid ||
            object.key != tombstone_key(descriptor, tombstone.value().operation_uuid)) {
            return base::Result<std::set<std::string, std::less<>>>::failure(
                tombstone ? scanner_error(base::ErrorCode::kConflict,
                                          "deletion tombstone identity conflicts with repository")
                          : tombstone.error());
        }
        for (const auto& target : tombstone.value().targets) {
            hidden.insert(target.file_uuid);
        }
    }
    return base::Result<std::set<std::string, std::less<>>>::success(std::move(hidden));
}

[[nodiscard]] base::Result<std::vector<CatalogEntry>>
load_entries(ports::IObjectReader& reader, ports::IPrefixEnumerator& enumerator,
             const RepositoryDescriptor& descriptor,
             const std::set<std::string, std::less<>>& hidden, const CatalogScannerLimits& limits,
             std::uint64_t& remaining, const base::CancellationToken cancellation) {
    auto objects = enumerate_all(enumerator, descriptor.catalog_prefix,
                                 limits.maximum_catalog_objects, cancellation);
    if (!objects) {
        return base::Result<std::vector<CatalogEntry>>::failure(objects.error());
    }
    std::vector<CatalogEntry> entries;
    for (const auto& object : objects.value()) {
        auto encoded = read_object(reader, object, limits.codec, remaining, cancellation);
        auto entry = encoded ? decode_catalog_entry_json(encoded.value(), limits.codec)
                             : base::Result<CatalogEntry>::failure(encoded.error());
        if (!entry || entry.value().repository_uuid != descriptor.repository_uuid ||
            object.key != catalog_key(descriptor, entry.value().file_uuid)) {
            return base::Result<std::vector<CatalogEntry>>::failure(
                entry ? scanner_error(base::ErrorCode::kConflict,
                                      "catalog entry identity conflicts with repository")
                      : entry.error());
        }
        if (!hidden.contains(entry.value().file_uuid)) {
            entries.push_back(std::move(entry).value());
        }
    }
    return base::Result<std::vector<CatalogEntry>>::success(std::move(entries));
}

[[nodiscard]] base::Result<CatalogScanPage> build_page(RepositoryDescriptor descriptor,
                                                       std::vector<CatalogEntry> entries,
                                                       const CatalogScanRequest& request) {
    auto graph = RecoveryPointGraph::build(entries);
    if (!graph) {
        return base::Result<CatalogScanPage>::failure(graph.error());
    }
    std::ranges::sort(entries, {}, &CatalogEntry::file_uuid);
    const auto after = request.continuation_token.value_or(std::string{});
    auto current = std::ranges::find_if(entries, [&](const CatalogEntry& entry) {
        return catalog_key(descriptor, entry.file_uuid) > after;
    });
    CatalogScanPage page;
    page.descriptor = std::move(descriptor);
    while (current != entries.end() && page.recovery_points.size() < request.maximum_results) {
        auto state = graph.value().chain_state(current->file_uuid);
        if (!state) {
            return base::Result<CatalogScanPage>::failure(state.error());
        }
        page.recovery_points.push_back({*current, state.value()});
        ++current;
    }
    if (current != entries.end() && !page.recovery_points.empty()) {
        page.continuation_token =
            catalog_key(page.descriptor, page.recovery_points.back().entry.file_uuid);
    }
    return base::Result<CatalogScanPage>::success(std::move(page));
}

[[nodiscard]] bool valid_request(const CatalogScanRequest& request,
                                 const CatalogScannerLimits& limits) {
    return request.maximum_results > 0 && request.maximum_results <= limits.maximum_page_results &&
           (!request.continuation_token ||
            (ports::is_valid_object_key(*request.continuation_token) &&
             request.continuation_token->starts_with("catalog/recovery-points/")));
}

} // namespace

RepositoryCatalogScanner::RepositoryCatalogScanner(ports::IObjectReader& reader,
                                                   ports::IPrefixEnumerator& enumerator,
                                                   CatalogScannerLimits limits)
    : reader_(reader), enumerator_(enumerator), limits_(std::move(limits)) {}

base::Result<CatalogScanPage>
RepositoryCatalogScanner::scan(const CatalogScanRequest& request,
                               const base::CancellationToken cancellation) const {
    if (!valid_request(request, limits_) || limits_.maximum_catalog_objects == 0 ||
        limits_.maximum_tombstone_objects == 0 || limits_.maximum_total_read_bytes == 0) {
        return base::Result<CatalogScanPage>::failure(
            scanner_error(base::ErrorCode::kInvalidArgument, "catalog scan request is invalid"));
    }
    std::uint64_t remaining = limits_.maximum_total_read_bytes;
    auto descriptor = load_descriptor(reader_, limits_, remaining, cancellation);
    if (!descriptor) {
        return base::Result<CatalogScanPage>::failure(descriptor.error());
    }
    auto hidden =
        load_hidden(reader_, enumerator_, descriptor.value(), limits_, remaining, cancellation);
    if (!hidden) {
        return base::Result<CatalogScanPage>::failure(hidden.error());
    }
    auto entries = load_entries(reader_, enumerator_, descriptor.value(), hidden.value(), limits_,
                                remaining, cancellation);
    return entries ? build_page(std::move(descriptor).value(), std::move(entries).value(), request)
                   : base::Result<CatalogScanPage>::failure(entries.error());
}

} // namespace aegra::personal_repository
