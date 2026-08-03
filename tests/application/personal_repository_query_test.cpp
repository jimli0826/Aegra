#include "aegra/application/personal_repository_query.h"

#include "aegra/adapters/memory/memory_object_storage.h"
#include "aegra/personal_repository/catalog.h"

#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace application = aegra::application;
namespace contracts = aegra::contracts;
namespace memory = aegra::adapters::memory;
namespace repository = aegra::personal_repository;
namespace ports = aegra::ports;

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

bool seed(memory::MemoryObjectStorage& storage) {
    repository::RepositoryDescriptor descriptor;
    descriptor.repository_uuid = kRepositoryUuid;
    auto encoded_descriptor = repository::encode_repository_descriptor_json(descriptor);
    repository::CatalogEntry entry;
    entry.repository_uuid = kRepositoryUuid;
    entry.file_uuid = kFileUuid;
    entry.backup_set_uuid = kSetUuid;
    entry.backup_type = aegra::format::BackupType::kFull;
    entry.archive_main_key = std::string("archives/2026/08/") + kFileUuid + ".bkf";
    entry.logical_size_bytes = 4'096;
    entry.stored_size_bytes = 2'048;
    entry.source_count = 1;
    auto encoded_entry = repository::encode_catalog_entry_json(entry);
    return encoded_descriptor && encoded_entry &&
           publish(storage, "staging/query/descriptor", "aegra.repository",
                   encoded_descriptor.value()) &&
           publish(storage, "staging/query/entry",
                   std::string("catalog/recovery-points/") + kFileUuid + ".entry",
                   encoded_entry.value());
}

bool test_unconfigured_query() {
    application::PersonalRepositoryQuery query;
    auto page = query.list_recovery_points({25, std::nullopt}, {});
    return expect(page && page.value().state == contracts::RepositoryCatalogState::kNotConfigured &&
                      page.value().repository_uuid.empty() && page.value().items.empty(),
                  "unconfigured application query returns a valid empty page");
}

bool test_catalog_mapping_and_validation() {
    memory::MemoryObjectStorage storage;
    if (!expect(seed(storage), "application query fixture publishes")) {
        return false;
    }
    application::PersonalRepositoryQuery query(storage, storage);
    auto page = query.list_recovery_points({25, std::nullopt}, {});
    bool passed = expect(
        page && page.value().state == contracts::RepositoryCatalogState::kCatalogReady &&
            page.value().repository_uuid == kRepositoryUuid && page.value().items.size() == 1,
        "application maps scanner output to a catalog-ready page");
    if (page) {
        const auto& item = page.value().items.front();
        passed &= expect(item.file_uuid == kFileUuid &&
                             item.backup_type == contracts::PersonalBackupType::kFull &&
                             item.chain_state == contracts::RecoveryPointChainState::kComplete &&
                             item.logical_size_bytes == 4'096,
                         "application maps recovery point identity, type, chain, and sizes");
    }
    passed &= expect(!query.list_recovery_points({0, std::nullopt}, {}),
                     "application rejects invalid page requests before storage access");
    return passed;
}

} // namespace

int main() noexcept {
    try {
        return test_unconfigured_query() && test_catalog_mapping_and_validation() ? EXIT_SUCCESS
                                                                                  : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
