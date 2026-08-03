#include "aegra/personal_repository/catalog_scanner.h"

#include "aegra/adapters/memory/memory_object_storage.h"
#include "aegra/base/error.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace memory = aegra::adapters::memory;
namespace repository = aegra::personal_repository;
namespace ports = aegra::ports;
using aegra::format::BackupType;

constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFullUuid = "11111111-2222-4333-8444-555555555555";
constexpr auto kIncrementalUuid = "22222222-3333-4444-8555-666666666666";
constexpr auto kOrphanUuid = "33333333-4444-4555-8666-777777777777";
constexpr auto kMissingUuid = "44444444-5555-4666-8777-888888888888";
constexpr auto kOperationUuid = "99999999-8888-4777-8666-555555555555";

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

repository::RepositoryDescriptor descriptor() {
    repository::RepositoryDescriptor value;
    value.repository_uuid = kRepositoryUuid;
    value.created_utc_ms = 1'785'600'000'000ULL;
    return value;
}

repository::CatalogEntry entry(std::string uuid, const BackupType type,
                               std::optional<std::string> parent = std::nullopt) {
    repository::CatalogEntry value;
    value.repository_uuid = kRepositoryUuid;
    value.file_uuid = std::move(uuid);
    value.backup_set_uuid = kSetUuid;
    value.parent_uuid = std::move(parent);
    value.backup_type = type;
    value.archive_main_key = "archives/2026/08/" + value.file_uuid + ".bkf";
    value.has_sidecar = true;
    value.created_utc_ms = 1'785'600'000'000ULL;
    value.logical_size_bytes = 4'096;
    value.stored_size_bytes = 2'048;
    value.source_count = 1;
    return value;
}

std::string catalog_key(const std::string_view uuid) {
    return "catalog/recovery-points/" + std::string(uuid) + ".entry";
}

bool publish(memory::MemoryObjectStorage& storage, const std::string_view key,
             const std::string_view contents) {
    static std::atomic_uint32_t sequence{0};
    const auto staging = "staging/scanner/" + std::to_string(sequence.fetch_add(1));
    auto writer = storage.begin_staged_write(staging, {});
    if (!writer) {
        return false;
    }
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    if (!writer.value()->write(bytes, {}) || !writer.value()->complete({})) {
        return false;
    }
    return storage
        .publish({staging, std::string(key), ports::PublishCondition::kCreateOnly, std::nullopt},
                 {})
        .has_value();
}

bool publish_descriptor(memory::MemoryObjectStorage& storage) {
    auto encoded = repository::encode_repository_descriptor_json(descriptor());
    return encoded && publish(storage, "aegra.repository", encoded.value());
}

bool publish_entry(memory::MemoryObjectStorage& storage, const repository::CatalogEntry& value) {
    auto encoded = repository::encode_catalog_entry_json(value);
    return encoded && publish(storage, catalog_key(value.file_uuid), encoded.value());
}

bool seed_catalog(memory::MemoryObjectStorage& storage) {
    return publish_descriptor(storage) &&
           publish_entry(storage, entry(kFullUuid, BackupType::kFull)) &&
           publish_entry(storage, entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid)) &&
           publish_entry(storage, entry(kOrphanUuid, BackupType::kIncremental, kMissingUuid));
}

bool test_paging_short_reads_and_chain_state() {
    memory::MemoryObjectStorageOptions options;
    options.maximum_read_size = 7;
    memory::MemoryObjectStorage storage(options);
    if (!expect(seed_catalog(storage), "catalog fixture publishes")) {
        return false;
    }
    repository::RepositoryCatalogScanner scanner(storage, storage);
    auto first = scanner.scan({std::nullopt, 2}, {});
    bool passed = expect(first && first.value().descriptor.repository_uuid == kRepositoryUuid &&
                             first.value().recovery_points.size() == 2 &&
                             first.value().recovery_points[0].entry.file_uuid == kFullUuid &&
                             first.value().recovery_points[1].chain_state ==
                                 repository::ChainState::kComplete &&
                             first.value().continuation_token.has_value(),
                         "scanner handles short reads and returns a stable first page");
    if (!first || !first.value().continuation_token) {
        return false;
    }
    auto second = scanner.scan({first.value().continuation_token, 2}, {});
    passed &= expect(second && second.value().recovery_points.size() == 1 &&
                         second.value().recovery_points.front().entry.file_uuid == kOrphanUuid &&
                         second.value().recovery_points.front().chain_state ==
                             repository::ChainState::kIncomplete &&
                         !second.value().continuation_token,
                     "scanner continues after an opaque token and preserves incomplete chains");
    return passed;
}

bool test_tombstone_hides_target() {
    memory::MemoryObjectStorage storage;
    if (!seed_catalog(storage)) {
        return false;
    }
    auto target = entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    repository::DeletionTombstone tombstone;
    tombstone.repository_uuid = kRepositoryUuid;
    tombstone.operation_uuid = kOperationUuid;
    tombstone.created_utc_ms = 1'785'600'000'000ULL;
    tombstone.targets.push_back({target.file_uuid,
                                 target.catalog_generation,
                                 target.archive_main_key,
                                 {target.archive_main_key + ".bhx", target.archive_main_key}});
    auto encoded = repository::encode_deletion_tombstone_json(tombstone);
    const auto key = std::string("catalog/deletions/") + kOperationUuid + ".tombstone";
    if (!expect(encoded && publish(storage, key, encoded.value()), "tombstone fixture publishes")) {
        return false;
    }
    repository::RepositoryCatalogScanner scanner(storage, storage);
    auto page = scanner.scan({std::nullopt, 10}, {});
    return expect(page && page.value().recovery_points.size() == 2 &&
                      page.value().recovery_points[0].entry.file_uuid == kFullUuid &&
                      page.value().recovery_points[1].entry.file_uuid == kOrphanUuid,
                  "valid tombstone hides its recovery point from catalog queries");
}

bool test_corruption_limits_and_cancellation() {
    memory::MemoryObjectStorage conflict_storage;
    auto full = entry(kFullUuid, BackupType::kFull);
    auto encoded = repository::encode_catalog_entry_json(full);
    const auto wrong_key = catalog_key(kIncrementalUuid);
    bool seeded = publish_descriptor(conflict_storage) && encoded &&
                  publish(conflict_storage, wrong_key, encoded.value());
    repository::RepositoryCatalogScanner conflict_scanner(conflict_storage, conflict_storage);
    auto conflict = seeded ? conflict_scanner.scan({std::nullopt, 10}, {})
                           : aegra::base::Result<repository::CatalogScanPage>::failure(
                                 {aegra::base::ErrorCode::kInternal, "fixture failed"});
    bool passed = expect(!conflict && conflict.error().code == aegra::base::ErrorCode::kConflict,
                         "catalog key and content UUID conflict is rejected");

    memory::MemoryObjectStorage corrupt_storage;
    seeded = publish_descriptor(corrupt_storage) &&
             publish(corrupt_storage, catalog_key(kFullUuid), "{");
    repository::RepositoryCatalogScanner corrupt_scanner(corrupt_storage, corrupt_storage);
    auto corrupt = seeded ? corrupt_scanner.scan({std::nullopt, 10}, {})
                          : aegra::base::Result<repository::CatalogScanPage>::failure(
                                {aegra::base::ErrorCode::kInternal, "fixture failed"});
    passed &= expect(!corrupt && corrupt.error().code == aegra::base::ErrorCode::kCorruptData,
                     "damaged catalog JSON is rejected");

    repository::CatalogScannerLimits limits;
    limits.maximum_total_read_bytes = 8;
    repository::RepositoryCatalogScanner limited(corrupt_storage, corrupt_storage, limits);
    passed &= expect(!limited.scan({std::nullopt, 10}, {}), "total scan byte limit is enforced");
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = corrupt_scanner.scan({std::nullopt, 10}, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "pre-cancelled scan performs no successful read");
    return passed;
}

int run_tests() {
    return test_paging_short_reads_and_chain_state() && test_tombstone_hides_target() &&
                   test_corruption_limits_and_cancellation()
               ? EXIT_SUCCESS
               : EXIT_FAILURE;
}

} // namespace

int main() noexcept {
    try {
        return run_tests();
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
