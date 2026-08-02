#include "aegra/personal_repository/catalog.h"
#include "aegra/personal_repository/chain_graph.h"

#include "aegra/base/error.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace repository = aegra::personal_repository;
using aegra::format::BackupType;

constexpr auto kRepositoryUuid = "01234567-89ab-4cde-8f01-23456789abcd";
constexpr auto kSetUuid = "aaaaaaaa-bbbb-4ccc-8ddd-eeeeeeeeeeee";
constexpr auto kFullUuid = "11111111-2222-4333-8444-555555555555";
constexpr auto kIncrementalUuid = "22222222-3333-4444-8555-666666666666";
constexpr auto kLeafUuid = "33333333-4444-4555-8666-777777777777";
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

repository::DeletionTombstone tombstone() {
    repository::DeletionTombstone value;
    value.repository_uuid = kRepositoryUuid;
    value.operation_uuid = kOperationUuid;
    value.created_utc_ms = 1'785'600'000'000ULL;
    const auto main_key = std::string("archives/2026/08/") + kFullUuid + ".bkf";
    value.targets.push_back({kFullUuid,
                             1,
                             main_key,
                             {main_key + ".bhx", main_key + ".002", main_key + ".001", main_key}});
    return value;
}

bool test_codec_roundtrips() {
    auto encoded_descriptor = repository::encode_repository_descriptor_json(descriptor());
    bool passed = expect(encoded_descriptor.has_value(), "repository descriptor encodes");
    if (!encoded_descriptor) {
        return false;
    }
    auto decoded_descriptor =
        repository::decode_repository_descriptor_json(encoded_descriptor.value());
    passed &= expect(decoded_descriptor && decoded_descriptor.value() == descriptor(),
                     "repository descriptor roundtrips");

    auto full = entry(kFullUuid, BackupType::kFull);
    auto encoded_entry = repository::encode_catalog_entry_json(full);
    passed &= expect(encoded_entry.has_value(), "catalog entry encodes");
    if (encoded_entry) {
        auto decoded_entry = repository::decode_catalog_entry_json(encoded_entry.value());
        passed &=
            expect(decoded_entry && decoded_entry.value() == full, "catalog entry roundtrips");
    }

    auto deletion = tombstone();
    auto encoded_deletion = repository::encode_deletion_tombstone_json(deletion);
    passed &= expect(encoded_deletion.has_value(), "deletion tombstone encodes");
    if (encoded_deletion) {
        auto decoded_deletion =
            repository::decode_deletion_tombstone_json(encoded_deletion.value());
        passed &= expect(decoded_deletion && decoded_deletion.value() == deletion,
                         "deletion tombstone roundtrips");
    }
    return passed;
}

bool test_codec_rejections() {
    const std::string duplicate =
        R"({"schema_version":1,"schema_version":1,"kind":"aegra_personal_repository"})";
    auto duplicate_result = repository::decode_repository_descriptor_json(duplicate);
    bool passed = expect(!duplicate_result &&
                             duplicate_result.error().code == aegra::base::ErrorCode::kCorruptData,
                         "duplicate JSON keys are rejected");

    auto encoded = repository::encode_repository_descriptor_json(descriptor());
    std::string unknown = encoded.value();
    unknown.insert(unknown.size() - 1, R"(,"unexpected":true)");
    passed &= expect(!repository::decode_repository_descriptor_json(unknown),
                     "unknown descriptor key is rejected");

    std::string negative = encoded.value();
    const auto created_position = negative.find("1785600000000");
    negative.replace(created_position, std::string("1785600000000").size(), "-1");
    passed &= expect(!repository::decode_repository_descriptor_json(negative),
                     "negative unsigned field is rejected");

    std::string overflow = encoded.value();
    const auto layout_position = overflow.find(R"("layout_version":1)");
    overflow.replace(layout_position, std::string(R"("layout_version":1)").size(),
                     R"("layout_version":4294967296)");
    passed &= expect(!repository::decode_repository_descriptor_json(overflow),
                     "uint32 overflow is rejected");

    auto unsafe = entry(kFullUuid, BackupType::kFull);
    unsafe.archive_main_key = "archives/../escape.bkf";
    passed &= expect(!repository::encode_catalog_entry_json(unsafe),
                     "repository key traversal is rejected");

    auto invalid_layout = entry(kFullUuid, BackupType::kFull);
    invalid_layout.archive_main_key = "archives/misc/" + invalid_layout.file_uuid + ".bkf";
    passed &= expect(!repository::encode_catalog_entry_json(invalid_layout),
                     "noncanonical archive layout is rejected");

    auto invalid_parent = entry(kFullUuid, BackupType::kFull, kIncrementalUuid);
    passed &= expect(!repository::encode_catalog_entry_json(invalid_parent),
                     "full catalog entry parent is rejected");

    auto excessive_parts = entry(kFullUuid, BackupType::kFull);
    excessive_parts.split_part_count = repository::kMaximumSplitPartCount + 1;
    passed &= expect(!repository::encode_catalog_entry_json(excessive_parts),
                     "excessive split part count is rejected");

    auto invalid_deletion = tombstone();
    std::swap(invalid_deletion.targets.front().member_keys[1],
              invalid_deletion.targets.front().member_keys[2]);
    passed &= expect(!repository::encode_deletion_tombstone_json(invalid_deletion),
                     "out-of-order split deletion is rejected");

    auto missing_part = tombstone();
    missing_part.targets.front().member_keys.erase(
        missing_part.targets.front().member_keys.begin() + 2);
    passed &= expect(!repository::encode_deletion_tombstone_json(missing_part),
                     "deletion tombstone missing split part is rejected");

    repository::CatalogCodecLimits limits;
    limits.maximum_document_bytes = 8;
    passed &= expect(!repository::decode_repository_descriptor_json(encoded.value(), limits),
                     "document size limit is enforced");
    return passed;
}

bool test_chain_resolution() {
    auto full = entry(kFullUuid, BackupType::kFull);
    auto incremental = entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    auto leaf = entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    auto graph = repository::RecoveryPointGraph::build({leaf, full, incremental});
    bool passed = expect(graph.has_value(), "valid chain graph builds");
    if (!graph) {
        return false;
    }
    auto chain = graph.value().resolve_chain(kLeafUuid);
    passed &=
        expect(chain && chain.value().size() == 3 && chain.value()[0].file_uuid == kFullUuid &&
                   chain.value()[2].file_uuid == kLeafUuid,
               "chain resolves base-first by UUID relation");
    auto state = graph.value().chain_state(kLeafUuid);
    passed &= expect(state && state.value() == repository::ChainState::kComplete,
                     "complete chain reports complete state");
    passed &= expect(repository::RecoveryPointGraph::build({full, incremental}, 2).has_value(),
                     "chain at maximum depth is accepted");
    passed &= expect(!repository::RecoveryPointGraph::build({full, incremental, leaf}, 2),
                     "chain exceeding maximum depth is rejected");
    return passed;
}

bool test_incomplete_and_invalid_graphs() {
    auto empty = repository::RecoveryPointGraph::build({});
    bool passed = expect(empty.has_value(), "empty repository graph is valid");

    auto missing_parent = entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    auto incomplete = repository::RecoveryPointGraph::build({missing_parent});
    passed &= expect(incomplete.has_value(), "missing parent remains discoverable");
    if (incomplete) {
        auto state = incomplete.value().chain_state(kLeafUuid);
        passed &= expect(state && state.value() == repository::ChainState::kIncomplete,
                         "missing parent reports incomplete chain");
        passed &= expect(!incomplete.value().resolve_chain(kLeafUuid),
                         "missing parent cannot resolve restore chain");
    }

    auto duplicate = entry(kFullUuid, BackupType::kFull);
    passed &= expect(!repository::RecoveryPointGraph::build({duplicate, duplicate}),
                     "duplicate recovery point UUID is rejected");

    auto parent = entry(kFullUuid, BackupType::kFull);
    auto cross_set = entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    cross_set.backup_set_uuid = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    passed &= expect(!repository::RecoveryPointGraph::build({parent, cross_set}),
                     "cross-backup-set parent is rejected");

    auto cross_repository = entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    cross_repository.repository_uuid = "bbbbbbbb-cccc-4ddd-8eee-ffffffffffff";
    passed &= expect(!repository::RecoveryPointGraph::build({parent, cross_repository}),
                     "cross-repository graph is rejected");

    auto first = entry(kIncrementalUuid, BackupType::kIncremental, kLeafUuid);
    auto second = entry(kLeafUuid, BackupType::kIncremental, kIncrementalUuid);
    passed &= expect(!repository::RecoveryPointGraph::build({first, second}),
                     "recovery point cycle is rejected");

    auto differential_parent = entry(kIncrementalUuid, BackupType::kIncremental, kFullUuid);
    auto differential = entry(kLeafUuid, BackupType::kDifferential, kIncrementalUuid);
    passed &=
        expect(!repository::RecoveryPointGraph::build({parent, differential_parent, differential}),
               "differential parent must be full");
    return passed;
}

int run_tests() {
    const bool passed = test_codec_roundtrips() && test_codec_rejections() &&
                        test_chain_resolution() && test_incomplete_and_invalid_graphs();
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
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
