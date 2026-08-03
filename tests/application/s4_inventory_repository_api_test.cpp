#include "aegra/adapters/memory/memory_object_storage.h"
#include "aegra/adapters/sqlite/sqlite_control_plane.h"
#include "aegra/application/connected_repository_query.h"
#include "aegra/application/repository_connection_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/personal_repository/catalog.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"
#include "aegra/ports/source_inventory.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace application = aegra::application;
namespace base = aegra::base;
namespace contracts = aegra::contracts;
namespace memory = aegra::adapters::memory;
namespace ports = aegra::ports;
namespace repository = aegra::personal_repository;
namespace sqlite_cp = aegra::adapters::sqlite;

constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFileUuid = "11111111-2222-4333-8444-555555555555";

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-s4-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                 std::to_string(sequence.fetch_add(1)));
        std::filesystem::create_directories(path_);
    }
    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }
    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;
    [[nodiscard]] std::filesystem::path database_path() const { return path_ / "control.db"; }

  private:
    std::filesystem::path path_;
};

class FakeClock final : public ports::IClock {
  public:
    explicit FakeClock(const std::int64_t start) noexcept : now_(start) {}
    [[nodiscard]] std::int64_t now_utc_ms() const noexcept override { return now_; }
    void advance(const std::int64_t delta) noexcept { now_ += delta; }

  private:
    mutable std::int64_t now_;
};

class FakeRandom final : public ports::IRandomSource {
  public:
    [[nodiscard]] base::Result<void> fill(const std::span<std::byte> destination,
                                          const base::CancellationToken& cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure({base::ErrorCode::kCancelled, "random cancelled"});
        }
        for (auto& byte : destination) {
            byte = static_cast<std::byte>(next_++);
        }
        return base::Result<void>::success();
    }

  private:
    std::uint8_t next_{1};
};

class FakeInventory final : public ports::ISourceInventory {
  public:
    std::vector<ports::SourceInventoryRecord> records;
    bool fail{false};
    bool cancel_requested{false};

    [[nodiscard]] base::Result<std::vector<ports::SourceInventoryRecord>>
    list_sources(const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested() || cancel_requested) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kCancelled, "inventory cancelled"});
        }
        if (fail) {
            return base::Result<std::vector<ports::SourceInventoryRecord>>::failure(
                {base::ErrorCode::kIoFailure, "inventory unavailable"});
        }
        return base::Result<std::vector<ports::SourceInventoryRecord>>::success(records);
    }
};

class MemoryStorageAccess final : public ports::IRepositoryStorageAccess {
  public:
    explicit MemoryStorageAccess(std::shared_ptr<memory::MemoryObjectStorage> storage)
        : storage_(std::move(storage)) {}
    [[nodiscard]] ports::IObjectReader& reader() noexcept override { return *storage_; }
    [[nodiscard]] ports::IPrefixEnumerator& enumerator() noexcept override { return *storage_; }

  private:
    std::shared_ptr<memory::MemoryObjectStorage> storage_;
};

class ShortReadStorageAccess final : public ports::IRepositoryStorageAccess,
                                     private ports::IObjectReader {
  public:
    explicit ShortReadStorageAccess(std::shared_ptr<memory::MemoryObjectStorage> storage)
        : storage_(std::move(storage)) {}

    [[nodiscard]] ports::IObjectReader& reader() noexcept override { return *this; }
    [[nodiscard]] ports::IPrefixEnumerator& enumerator() noexcept override { return *storage_; }

  private:
    [[nodiscard]] base::Result<ports::ObjectAttributes>
    get_attributes(const std::string_view key,
                   const base::CancellationToken cancellation) override {
        return storage_->get_attributes(key, cancellation);
    }

    [[nodiscard]] base::Result<std::size_t>
    read_range(const std::string_view key, const std::uint64_t offset,
               const std::span<std::byte> destination,
               const base::CancellationToken cancellation) override {
        return storage_->read_range(
            key, offset, destination.first((std::min)(destination.size(), std::size_t{3})),
            cancellation);
    }

    std::shared_ptr<memory::MemoryObjectStorage> storage_;
};

class FakeStorageFactory final : public ports::IRepositoryStorageFactory {
  public:
    std::map<std::string, std::shared_ptr<memory::MemoryObjectStorage>, std::less<>> stores;
    std::map<std::string, base::Error, std::less<>> failures;
    bool short_reads{false};

    [[nodiscard]] base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>
    open(const std::string_view locator, const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                {base::ErrorCode::kCancelled, "open cancelled"});
        }
        const auto key = std::string(locator);
        if (const auto failed = failures.find(key); failed != failures.end()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                failed->second);
        }
        const auto found = stores.find(key);
        if (found == stores.end()) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::failure(
                {base::ErrorCode::kNotFound, "repository root not found"});
        }
        if (short_reads) {
            return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::success(
                std::make_unique<ShortReadStorageAccess>(found->second));
        }
        return base::Result<std::unique_ptr<ports::IRepositoryStorageAccess>>::success(
            std::make_unique<MemoryStorageAccess>(found->second));
    }
};

[[nodiscard]] contracts::ServiceRecoveryPointListRequest
rp_request(std::optional<std::string> connection_id, const std::uint32_t maximum_results = 25) {
    contracts::ServiceRecoveryPointListRequest request;
    request.repository_connection_id = std::move(connection_id);
    request.page.maximum_results = maximum_results;
    return request;
}

bool publish(memory::MemoryObjectStorage& storage, const std::string_view staging_key,
             const std::string_view destination_key, const std::string_view contents) {
    auto writer = storage.begin_staged_write(staging_key, {});
    if (!writer) {
        return false;
    }
    const auto bytes = std::as_bytes(std::span(contents.data(), contents.size()));
    return writer.value()->write(bytes, {}) && writer.value()->complete({}) &&
           storage
               .publish({std::string(staging_key), std::string(destination_key),
                         ports::PublishCondition::kCreateOnly, std::nullopt},
                        {})
               .has_value();
}

bool seed_repository(memory::MemoryObjectStorage& storage, const std::string_view file_uuid) {
    repository::RepositoryDescriptor descriptor;
    descriptor.repository_uuid = kRepositoryUuid;
    auto encoded_descriptor = repository::encode_repository_descriptor_json(descriptor);
    repository::CatalogEntry entry;
    entry.repository_uuid = kRepositoryUuid;
    entry.file_uuid = std::string(file_uuid);
    entry.backup_set_uuid = kSetUuid;
    entry.backup_type = aegra::format::BackupType::kFull;
    entry.archive_main_key = std::string("archives/2026/08/") + std::string(file_uuid) + ".bkf";
    entry.logical_size_bytes = 4'096;
    entry.stored_size_bytes = 2'048;
    entry.source_count = 1;
    auto encoded_entry = repository::encode_catalog_entry_json(entry);
    return encoded_descriptor && encoded_entry &&
           publish(storage, "staging/s4/descriptor", "aegra.repository",
                   encoded_descriptor.value()) &&
           publish(storage, "staging/s4/entry",
                   std::string("catalog/recovery-points/") + std::string(file_uuid) + ".entry",
                   encoded_entry.value());
}

[[nodiscard]] base::Result<std::unique_ptr<sqlite_cp::SqliteControlPlaneDatabase>>
open_control_plane(const std::filesystem::path& path) {
    return sqlite_cp::SqliteControlPlaneDatabase::open(
        {path, sqlite_cp::SqliteOpenMode::kCreateIfMissing});
}

bool test_inventory_opaque_ids_and_pagination() {
    FakeInventory inventory;
    inventory.records = {
        {.source_id = "vol.bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb",
         .stable_key = R"(\\?\Volume{bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb}\)",
         .display_name = "Data",
         .kind = contracts::SourceKind::kVolume,
         .availability = contracts::SourceAvailability::kAvailable,
         .capacity_bytes = 1'000'000,
         .is_system = false,
         .is_read_only = false},
        {.source_id = "vol.aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa",
         .stable_key = R"(\\?\Volume{aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa}\)",
         .display_name = "System",
         .kind = contracts::SourceKind::kVolume,
         .availability = contracts::SourceAvailability::kAvailable,
         .capacity_bytes = 2'000'000,
         .is_system = true,
         .is_read_only = false},
        {.source_id = "vol.offline",
         .stable_key = "offline-volume",
         .display_name = "Offline",
         .kind = contracts::SourceKind::kVolume,
         .availability = contracts::SourceAvailability::kUnavailable,
         .capacity_bytes = 0,
         .is_system = false,
         .is_read_only = true},
    };
    application::SourceInventoryQuery query(inventory);
    auto page = query.list_sources({{1, std::nullopt}, true}, {});
    bool passed = expect(page && page.value().items.size() == 1 &&
                             page.value().continuation_token.has_value(),
                         "inventory page size and token");
    if (!page) {
        return false;
    }
    const auto& first = page.value().items.front();
    passed &= expect(first.source_id.rfind("vol.", 0) == 0, "source id is opaque vol. prefix");
    passed &= expect(first.source_id.find('\\') == std::string::npos &&
                         first.source_id.find('?') == std::string::npos,
                     "source id does not contain device path characters");
    auto second = query.list_sources({{10, page.value().continuation_token}, true}, {});
    passed &= expect(second && second.value().items.size() == 2, "inventory continuation works");
    auto available_only = query.list_sources({{10, std::nullopt}, false}, {});
    passed &= expect(available_only && available_only.value().items.size() == 2,
                     "unavailable sources can be filtered out");
    if (available_only) {
        for (const auto& item : available_only.value().items) {
            if (item.is_system) {
                passed &= expect(!item.is_selectable, "system volume is not selectable");
            }
        }
    }
    auto wrong_token = query.list_sources({{10, std::string{"inv|0|vol.x"}}, true}, {});
    passed &= expect(!wrong_token, "token filter mismatch is rejected");
    auto resolved = query.resolve_source("vol.bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb", {});
    passed &= expect(resolved && resolved.value().stable_key ==
                                     R"(\\?\Volume{bbbbbbbb-bbbb-4bbb-8bbb-bbbbbbbbbbbb}\)",
                     "opaque source id resolves to the trusted stable key");
    auto system = query.resolve_source("vol.aaaaaaaa-aaaa-4aaa-8aaa-aaaaaaaaaaaa", {});
    passed &= expect(!system && system.error().code == base::ErrorCode::kConflict,
                     "non-selectable source cannot be resolved for a job");
    return passed;
}

bool test_inventory_duplicate_ids() {
    FakeInventory inventory;
    inventory.records = {
        {.source_id = "vol.duplicate",
         .stable_key = "first",
         .display_name = "First",
         .kind = contracts::SourceKind::kVolume,
         .availability = contracts::SourceAvailability::kAvailable},
        {.source_id = "vol.duplicate",
         .stable_key = "second",
         .display_name = "Second",
         .kind = contracts::SourceKind::kVolume,
         .availability = contracts::SourceAvailability::kAvailable},
    };
    application::SourceInventoryQuery query(inventory);
    auto page = query.list_sources({{10, std::nullopt}, true}, {});
    return expect(!page && page.error().code == base::ErrorCode::kConflict,
                  "duplicate inventory source ids are rejected");
}

bool test_inventory_cancel() {
    FakeInventory inventory;
    inventory.cancel_requested = true;
    application::SourceInventoryQuery query(inventory);
    auto page = query.list_sources({{10, std::nullopt}, true}, {});
    return expect(!page && page.error().code == base::ErrorCode::kCancelled,
                  "inventory cancel surfaces");
}

bool test_repository_connection_lifecycle_and_rp_query() {
    TemporaryDirectory directory;
    auto control = open_control_plane(directory.database_path());
    if (!expect(control.has_value(), "control plane opens")) {
        return false;
    }
    FakeClock clock(1'700'000'000'000);
    FakeRandom random;
    FakeStorageFactory factory;
    auto store_a = std::make_shared<memory::MemoryObjectStorage>();
    auto store_b = std::make_shared<memory::MemoryObjectStorage>();
    if (!expect(seed_repository(*store_a, kFileUuid) &&
                    seed_repository(*store_b, "22222222-2222-4222-8222-222222222222"),
                "seed two repositories")) {
        return false;
    }
    factory.stores.emplace("mem://repo-a", store_a);
    factory.stores.emplace("mem://repo-b", store_b);

    application::RepositoryConnectionService connections(*control.value(), factory, clock, random);
    application::ConnectedRepositoryQuery query(*control.value(), factory);

    auto empty = query.list_recovery_points(rp_request(std::nullopt), {});
    bool passed = expect(empty && empty.value().catalog.state ==
                                      contracts::RepositoryCatalogState::kNotConfigured,
                         "no connection yields not_configured");

    contracts::RepositoryConnectionInput input_a;
    input_a.display_name = "Repo A";
    input_a.locator = "mem://repo-a";
    input_a.credential_ref = contracts::SecretRef{"wincred://repo-a"};
    auto added = connections.add_connection(input_a, "idem-add-a", {});
    passed &=
        expect(added && added.value().disposition == contracts::CommandDisposition::kAccepted &&
                   added.value().resource_id.has_value(),
               "add connection accepted");
    if (!added || !added.value().resource_id) {
        return false;
    }
    const auto connection_a = *added.value().resource_id;

    auto same_key = connections.add_connection(input_a, "idem-add-a", {});
    passed &= expect(same_key &&
                         same_key.value().disposition == contracts::CommandDisposition::kReplayed &&
                         same_key.value().command_id == added.value().command_id &&
                         same_key.value().resource_id == added.value().resource_id,
                     "same idempotency key replays the durable acknowledgement");
    auto changed_request = input_a;
    changed_request.display_name = "Repo A changed";
    auto mismatched_key = connections.add_connection(changed_request, "idem-add-a", {});
    passed &= expect(!mismatched_key && mismatched_key.error().code == base::ErrorCode::kConflict,
                     "same idempotency key rejects a different request");

    auto replay = connections.add_connection(input_a, "idem-add-a-2", {});
    passed &=
        expect(replay && replay.value().disposition == contracts::CommandDisposition::kReplayed,
               "same locator/display replays");

    contracts::RepositoryConnectionInput conflict_input;
    conflict_input.display_name = "Other Name";
    conflict_input.locator = "mem://repo-a";
    auto conflict = connections.add_connection(conflict_input, "idem-add-conflict", {});
    passed &= expect(!conflict && conflict.error().code == base::ErrorCode::kConflict,
                     "same locator different name conflicts");

    contracts::ResourceRef ref_a;
    ref_a.resource_id = connection_a;
    auto tested = connections.test_connection(ref_a, "idem-test-a", {});
    passed &= expect(tested.has_value(), "test makes repository available");
    factory.failures.emplace("mem://repo-a",
                             base::Error{base::ErrorCode::kUnauthorized, "credential denied"});
    auto unauthorized = connections.test_connection(ref_a, "idem-test-unauthorized", {});
    passed &= expect(!unauthorized && unauthorized.error().code == base::ErrorCode::kUnauthorized,
                     "connection test propagates authorization failures");
    factory.failures.erase("mem://repo-a");
    contracts::RepositoryConnectionListRequest list_request;
    list_request.page.maximum_results = 10;
    auto listed = connections.list_connections(list_request, {});
    passed &= expect(
        listed && listed.value().items.size() == 1 && listed.value().items[0].is_default &&
            listed.value().items[0].state == contracts::RepositoryConnectionState::kAvailable,
        "list returns default available connection without locator");

    auto default_rp = query.list_recovery_points(rp_request(std::nullopt), {});
    passed &= expect(default_rp &&
                         default_rp.value().catalog.state ==
                             contracts::RepositoryCatalogState::kCatalogReady &&
                         default_rp.value().catalog.items.size() == 1 &&
                         default_rp.value().repository_connection_id == connection_a,
                     "default connection recovery points resolve");

    contracts::RepositoryConnectionInput input_b;
    input_b.display_name = "Repo B";
    input_b.locator = "mem://repo-b";
    auto imported = connections.import_connection(input_b, "idem-import-b", {});
    passed &= expect(imported && imported.value().resource_id.has_value(), "import existing repo");
    if (!imported || !imported.value().resource_id) {
        return false;
    }
    const auto connection_b = *imported.value().resource_id;
    contracts::ResourceRef ref_b;
    ref_b.resource_id = connection_b;
    auto set_default = connections.set_default_connection(ref_b, "idem-default-b", {});
    passed &= expect(set_default.has_value(), "set default connection");
    auto by_id = query.list_recovery_points(rp_request(connection_a), {});
    passed &= expect(by_id && by_id.value().repository_connection_id == connection_a &&
                         by_id.value().catalog.items.size() == 1,
                     "explicit connection id selects repository");
    auto default_after = query.list_recovery_points(rp_request(std::nullopt), {});
    passed &=
        expect(default_after && default_after.value().repository_connection_id == connection_b,
               "default connection follows set_default");

    factory.failures.emplace("mem://repo-a", base::Error{base::ErrorCode::kNotFound, "offline"});
    auto offline = query.list_recovery_points(rp_request(connection_a), {});
    passed &= expect(offline && offline.value().catalog.state ==
                                    contracts::RepositoryCatalogState::kNotConfigured,
                     "offline repository returns not_configured page");

    auto removed = connections.remove_connection(ref_a, "idem-remove-a", {});
    passed &= expect(removed.has_value(), "remove connection control-plane only");
    auto missing = control.value()->get_repository_connection(connection_a, {});
    passed &= expect(missing && !missing.value(), "connection removed from control plane");
    // Memory store still has objects: remove does not delete repository content.
    passed &= expect(factory.stores.contains("mem://repo-a"),
                     "repository storage objects remain after connection remove");
    return passed;
}

bool test_import_rejects_missing_descriptor() {
    TemporaryDirectory directory;
    auto control = open_control_plane(directory.database_path());
    if (!control) {
        return false;
    }
    FakeClock clock(100);
    FakeRandom random;
    FakeStorageFactory factory;
    factory.stores.emplace("mem://empty", std::make_shared<memory::MemoryObjectStorage>());
    application::RepositoryConnectionService connections(*control.value(), factory, clock, random);
    contracts::RepositoryConnectionInput input;
    input.display_name = "Empty";
    input.locator = "mem://empty";
    auto imported = connections.import_connection(input, "idem-empty", {});
    return expect(!imported, "import without descriptor fails");
}

bool test_import_accepts_descriptor_short_reads() {
    TemporaryDirectory directory;
    auto control = open_control_plane(directory.database_path());
    if (!control)
        return false;
    FakeClock clock(150);
    FakeRandom random;
    FakeStorageFactory factory;
    factory.short_reads = true;
    auto store = std::make_shared<memory::MemoryObjectStorage>();
    if (!seed_repository(*store, kFileUuid))
        return false;
    factory.stores.emplace("mem://short", store);
    application::RepositoryConnectionService connections(*control.value(), factory, clock, random);
    contracts::RepositoryConnectionInput input;
    input.display_name = "Short reads";
    input.locator = "mem://short";
    auto imported = connections.import_connection(input, "idem-short", {});
    return expect(imported && imported.value().resource_id,
                  "repository descriptor supports repeated short reads");
}

bool test_corrupt_catalog_failure() {
    TemporaryDirectory directory;
    auto control = open_control_plane(directory.database_path());
    if (!control) {
        return false;
    }
    FakeClock clock(200);
    FakeRandom random;
    FakeStorageFactory factory;
    auto store = std::make_shared<memory::MemoryObjectStorage>();
    if (!publish(*store, "staging/bad/descriptor", "aegra.repository", "{not-json")) {
        return false;
    }
    factory.stores.emplace("mem://bad", store);
    application::RepositoryConnectionService connections(*control.value(), factory, clock, random);
    application::ConnectedRepositoryQuery query(*control.value(), factory);
    contracts::RepositoryConnectionInput input;
    input.display_name = "Bad";
    input.locator = "mem://bad";
    auto added = connections.add_connection(input, "idem-bad", {});
    if (!added || !added.value().resource_id) {
        return false;
    }
    auto page = query.list_recovery_points(rp_request(*added.value().resource_id), {});
    return expect(!page, "corrupt catalog fails the recovery point query");
}

} // namespace

int main() noexcept {
    try {
        const bool passed =
            test_inventory_opaque_ids_and_pagination() && test_inventory_duplicate_ids() &&
            test_inventory_cancel() && test_repository_connection_lifecycle_and_rp_query() &&
            test_import_rejects_missing_descriptor() &&
            test_import_accepts_descriptor_short_reads() && test_corrupt_catalog_failure();
        return passed ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
