#include "aegra/adapters/memory/memory_object_storage.h"

#include "ports/object_storage_contract.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>

namespace {

namespace memory = aegra::adapters::memory;
namespace ports = aegra::ports;
namespace tests = aegra::tests;

tests::ObjectStorageContractPorts port_set(memory::MemoryObjectStorage& storage) {
    return {storage, storage, storage, storage, storage, storage};
}

bool test_short_read_and_validation() {
    memory::MemoryObjectStorageOptions options;
    options.maximum_read_size = 2;
    memory::MemoryObjectStorage storage(options);
    auto ports = port_set(storage);
    const auto payload = tests::contract_bytes("short-read");
    auto published = tests::stage_and_publish(ports, "staging/memory/short",
                                              "archives/2026/08/short.bkf", payload);
    bool passed = tests::contract_expect(published.has_value(), "short-read fixture publishes");
    const auto capabilities = storage.capabilities();
    passed &= tests::contract_expect(
        capabilities.atomic_rename_publish && capabilities.conditional_create &&
            capabilities.strong_read_after_write && capabilities.strong_list_consistency,
        "memory storage reports its strong publication and consistency capabilities");
    std::array<std::byte, 8> destination{};
    auto read = storage.read_range("archives/2026/08/short.bkf", 0, destination, {});
    passed &= tests::contract_expect(read && read.value() == 2,
                                     "memory storage injects deterministic short reads");
    passed &= tests::contract_expect(!storage.begin_staged_write("../escape", {}),
                                     "memory storage rejects staging path traversal");
    passed &= tests::contract_expect(!storage.get_attributes("/absolute/object", {}),
                                     "memory storage rejects absolute object keys");
    passed &= tests::contract_expect(!storage.enumerate({"catalog/../", std::nullopt, 1}, {}),
                                     "memory storage rejects unsafe prefixes");
    return passed;
}

bool test_write_and_completion_failures() {
    memory::MemoryObjectStorageOptions write_options;
    write_options.fail_write_after_bytes = 2;
    memory::MemoryObjectStorage write_failure(write_options);
    auto session = write_failure.begin_staged_write("staging/memory/write-failure", {});
    const auto payload = tests::contract_bytes("abc");
    bool passed = tests::contract_expect(session.has_value(), "write failure session begins");
    if (session) {
        passed &= tests::contract_expect(
            !session.value()->write(payload, {}) &&
                !write_failure.get_attributes("staging/memory/write-failure", {}),
            "injected staged write failure does not publish partial data");
        session.value()->abort();
    }

    memory::MemoryObjectStorageOptions complete_options;
    complete_options.fail_complete = true;
    memory::MemoryObjectStorage complete_failure(complete_options);
    auto incomplete = complete_failure.begin_staged_write("staging/memory/complete-failure", {});
    if (!incomplete) {
        return false;
    }
    passed &= tests::contract_expect(incomplete.value()->write(payload, {}).has_value() &&
                                         !incomplete.value()->complete({}),
                                     "injected completion failure leaves no visible object");
    incomplete.value()->abort();
    return passed;
}

bool test_publish_outcomes() {
    const auto payload = tests::contract_bytes("publish");
    memory::MemoryObjectStorageOptions failure_options;
    failure_options.fail_publish_destination_key = "catalog/memory/fail";
    memory::MemoryObjectStorage failure(failure_options);
    auto failure_ports = port_set(failure);
    auto failed = tests::stage_and_publish(failure_ports, "staging/memory/fail",
                                           "catalog/memory/fail", payload);
    bool passed = tests::contract_expect(
        !failed && failed.error().code == aegra::base::ErrorCode::kIoFailure &&
            !failure.get_attributes("catalog/memory/fail", {}),
        "pre-publish failure leaves destination absent");

    memory::MemoryObjectStorageOptions unknown_options;
    unknown_options.unknown_publish_destination_key = "catalog/memory/unknown";
    memory::MemoryObjectStorage unknown(unknown_options);
    auto unknown_ports = port_set(unknown);
    auto result = tests::stage_and_publish(unknown_ports, "staging/memory/unknown",
                                           "catalog/memory/unknown", payload);
    passed &= tests::contract_expect(
        !result && result.error().code == aegra::base::ErrorCode::kOutcomeUnknown &&
            unknown.get_attributes("catalog/memory/unknown", {}).has_value(),
        "unknown publish outcome is reconcilable through attributes");
    return passed;
}

bool test_delete_outcomes() {
    const auto payload = tests::contract_bytes("delete");
    memory::MemoryObjectStorageOptions failure_options;
    failure_options.fail_delete_key = "catalog/memory/delete-fail";
    memory::MemoryObjectStorage failure(failure_options);
    auto failure_ports = port_set(failure);
    auto published = tests::stage_and_publish(failure_ports, "staging/memory/delete-fail",
                                              "catalog/memory/delete-fail", payload);
    if (!published) {
        return false;
    }
    auto failed = failure.delete_object(
        {published.value().key, "delete-failure", published.value().generation}, {});
    bool passed = tests::contract_expect(
        !failed && failed.error().code == aegra::base::ErrorCode::kIoFailure &&
            failure.get_attributes(published.value().key, {}).has_value(),
        "pre-delete failure leaves the object visible");

    memory::MemoryObjectStorageOptions unknown_options;
    unknown_options.unknown_delete_key = "catalog/memory/delete-unknown";
    memory::MemoryObjectStorage unknown(unknown_options);
    auto unknown_ports = port_set(unknown);
    auto unknown_object = tests::stage_and_publish(unknown_ports, "staging/memory/delete-unknown",
                                                   "catalog/memory/delete-unknown", payload);
    if (!unknown_object) {
        return false;
    }
    const ports::ObjectDeleteRequest request{unknown_object.value().key, "delete-unknown",
                                             unknown_object.value().generation};
    auto result = unknown.delete_object(request, {});
    passed &= tests::contract_expect(
        !result && result.error().code == aegra::base::ErrorCode::kOutcomeUnknown &&
            !unknown.get_attributes(unknown_object.value().key, {}),
        "unknown delete outcome is reconcilable through attributes");
    passed &= tests::contract_expect(unknown.delete_object(request, {}).has_value(),
                                     "unknown delete retries idempotently");
    return passed;
}

int run_tests() {
    memory::MemoryObjectStorage contract_storage;
    const bool passed = tests::run_object_storage_contract(port_set(contract_storage)) &&
                        test_short_read_and_validation() && test_write_and_completion_failures() &&
                        test_publish_outcomes() && test_delete_outcomes();
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
