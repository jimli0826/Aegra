#include "aegra/adapters/storage_local/local_object_storage.h"

#include "ports/object_storage_contract.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <system_error>

namespace {

namespace local = aegra::adapters::storage_local;
namespace tests = aegra::tests;

class TemporaryDirectory final {
  public:
    TemporaryDirectory() {
        static std::atomic_uint64_t sequence{0};
        const auto timestamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = std::filesystem::temp_directory_path() /
                ("aegra-storage-local-" + std::to_string(timestamp) + "-" +
                 std::to_string(sequence.fetch_add(1)));
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete;
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete;

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

  private:
    std::filesystem::path path_;
};

tests::ObjectStorageContractPorts port_set(local::LocalObjectStorage& storage) {
    return {storage, storage, storage, storage, storage, storage};
}

aegra::base::Result<std::unique_ptr<local::LocalObjectStorage>>
create_storage(const std::filesystem::path& root) {
    return local::LocalObjectStorage::open(
        {root, local::LocalRootMode::kCreateIfMissing, 4U * 1024U * 1024U});
}

bool test_shared_contract() {
    TemporaryDirectory directory;
    auto storage = create_storage(directory.path());
    return tests::contract_expect(storage.has_value(), "local storage root opens") &&
           tests::run_object_storage_contract(port_set(*storage.value()));
}

bool test_open_and_staging_recovery() {
    TemporaryDirectory directory;
    auto missing = local::LocalObjectStorage::open(
        {directory.path(), local::LocalRootMode::kOpenExisting, 1024});
    bool passed = tests::contract_expect(!missing, "open-existing rejects a missing root");
    auto storage = create_storage(directory.path());
    if (!storage) {
        return false;
    }
    const auto payload = tests::contract_bytes("restart-safe-staging");
    auto session = storage.value()->begin_staged_write("staging/restart/object", {});
    passed &= tests::contract_expect(session && session.value()->write(payload, {}) &&
                                         session.value()->complete({}),
                                     "completed staging object survives adapter lifetime");
    session.value().reset();
    storage.value().reset();

    auto reopened = local::LocalObjectStorage::open(
        {directory.path(), local::LocalRootMode::kOpenExisting, 1024});
    if (!reopened) {
        return false;
    }
    auto published =
        reopened.value()->publish({"staging/restart/object", "catalog/restart/object",
                                   aegra::ports::PublishCondition::kCreateOnly, std::nullopt},
                                  {});
    passed &= tests::contract_expect(published && published.value().size_bytes == payload.size(),
                                     "reopened adapter publishes completed staging data");
    if (!published) {
        return false;
    }
    const auto generation = published.value().generation;
    reopened.value().reset();

    auto second_open = local::LocalObjectStorage::open(
        {directory.path(), local::LocalRootMode::kOpenExisting, 1024});
    if (!second_open) {
        return false;
    }
    auto attributes = second_open.value()->get_attributes("catalog/restart/object", {});
    passed &= tests::contract_expect(attributes && attributes.value().generation == generation,
                                     "local generation is stable across process-style reopen");
    return passed;
}

bool test_abort_and_external_generation_change() {
    TemporaryDirectory directory;
    auto storage = create_storage(directory.path());
    if (!storage) {
        return false;
    }
    const auto payload = tests::contract_bytes("first");
    auto abandoned = storage.value()->begin_staged_write("staging/local/abandoned", {});
    bool passed = tests::contract_expect(abandoned && abandoned.value()->write(payload, {}),
                                         "abandoned local session writes partial data");
    abandoned.value().reset();
    auto retry = storage.value()->begin_staged_write("staging/local/abandoned", {});
    passed &= tests::contract_expect(retry.has_value(),
                                     "abandoned local session removes its internal partial");
    retry.value()->abort();

    auto ports = port_set(*storage.value());
    auto published = tests::stage_and_publish(ports, "staging/local/generation",
                                              "catalog/local/generation", payload);
    if (!published) {
        return false;
    }
    const auto object_path = directory.path() / "catalog" / "local" / "generation";
    std::ofstream output(object_path, std::ios::binary | std::ios::app);
    output.put('x');
    output.close();
    auto changed = storage.value()->get_attributes("catalog/local/generation", {});
    passed &= tests::contract_expect(changed && changed.value().size_bytes == payload.size() + 1 &&
                                         changed.value().generation != published.value().generation,
                                     "external file modification changes local generation");
    return passed;
}

bool test_path_and_read_only_safety() {
    TemporaryDirectory directory;
    auto storage = create_storage(directory.path());
    if (!storage) {
        return false;
    }
    bool passed = tests::contract_expect(!storage.value()->begin_staged_write("../escape", {}),
                                         "local storage rejects path traversal");
    passed &= tests::contract_expect(
        !storage.value()->begin_staged_write(".aegra-internal/writes/escape", {}),
        "local storage reserves its internal namespace");
    passed &= tests::contract_expect(
        !storage.value()->begin_staged_write(".AEGRA-INTERNAL/writes/escape", {}),
        "local storage reserves case aliases of its internal namespace");

    auto ports = port_set(*storage.value());
    const auto payload = tests::contract_bytes("read-only");
    auto published = tests::stage_and_publish(ports, "staging/local/read-only",
                                              "catalog/local/read-only", payload);
    if (!published) {
        return false;
    }
    const auto object_path = directory.path() / "catalog" / "local" / "read-only";
    const auto original_attributes = GetFileAttributesW(object_path.c_str());
    if (original_attributes == INVALID_FILE_ATTRIBUTES ||
        !SetFileAttributesW(object_path.c_str(), original_attributes | FILE_ATTRIBUTE_READONLY)) {
        return false;
    }
    auto removed = storage.value()->delete_object(
        {published.value().key, "read-only-delete", published.value().generation}, {});
    passed &= tests::contract_expect(!removed && std::filesystem::exists(object_path),
                                     "local delete preserves a read-only object on failure");
    SetFileAttributesW(object_path.c_str(), original_attributes);
    return passed;
}

bool test_reparse_point_safety_when_supported() {
    TemporaryDirectory directory;
    TemporaryDirectory target;
    auto storage = create_storage(directory.path());
    if (!storage) {
        return false;
    }
    std::error_code filesystem_error;
    std::filesystem::create_directories(target.path(), filesystem_error);
    if (filesystem_error) {
        return false;
    }
    const auto link = directory.path() / "staging-link";
    constexpr DWORD kAllowUnprivilegedCreate = 0x2;
    if (!CreateSymbolicLinkW(link.c_str(), target.path().c_str(),
                             SYMBOLIC_LINK_FLAG_DIRECTORY | kAllowUnprivilegedCreate)) {
        return true;
    }
    bool passed =
        tests::contract_expect(!storage.value()->begin_staged_write("staging-link/object", {}),
                               "local storage rejects a reparse-point object parent");
    auto nested_root = create_storage(link / "repository");
    passed &=
        tests::contract_expect(!nested_root, "local storage rejects a reparse-point root ancestor");
    return passed;
}

int run_tests() {
    const bool passed = test_shared_contract() && test_open_and_staging_recovery() &&
                        test_abort_and_external_generation_change() &&
                        test_path_and_read_only_safety() &&
                        test_reparse_point_safety_when_supported();
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
