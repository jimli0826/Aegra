#pragma once

#include "aegra/ports/object_storage.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace aegra::tests {

struct ObjectStorageContractPorts final {
    ports::IObjectReader& reader;
    ports::IStagedObjectWriter& staged_writer;
    ports::IPrefixEnumerator& enumerator;
    ports::IObjectPublisher& publisher;
    ports::IObjectDeleter& deleter;
    ports::IObjectStorageCapabilities& capability_provider;
};

[[nodiscard]] inline bool contract_expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

[[nodiscard]] inline std::vector<std::byte> contract_bytes(const std::string_view text) {
    std::vector<std::byte> result;
    result.reserve(text.size());
    for (const char character : text) {
        result.push_back(static_cast<std::byte>(character));
    }
    return result;
}

[[nodiscard]] inline base::Result<ports::ObjectAttributes>
stage_and_publish(ObjectStorageContractPorts& storage, const std::string_view staging_key,
                  const std::string_view destination_key, const std::span<const std::byte> data,
                  const ports::PublishCondition condition = ports::PublishCondition::kCreateOnly,
                  std::optional<std::string> expected_generation = std::nullopt) {
    auto session = storage.staged_writer.begin_staged_write(staging_key, {});
    if (!session) {
        return base::Result<ports::ObjectAttributes>::failure(session.error());
    }
    auto written = session.value()->write(data, {});
    if (!written) {
        return base::Result<ports::ObjectAttributes>::failure(written.error());
    }
    auto completed = session.value()->complete({});
    if (!completed) {
        return base::Result<ports::ObjectAttributes>::failure(completed.error());
    }
    return storage.publisher.publish({std::string(staging_key), std::string(destination_key),
                                      condition, std::move(expected_generation)},
                                     {});
}

[[nodiscard]] inline bool test_stage_publish_and_read(ObjectStorageContractPorts& storage) {
    const auto payload = contract_bytes("repository-object");
    auto session = storage.staged_writer.begin_staged_write("staging/contract/base", {});
    bool passed = contract_expect(session.has_value(), "storage begins a staged write");
    if (!session) {
        return false;
    }
    passed &= contract_expect(session.value()->write(std::span(payload).first(5), {}).has_value(),
                              "staged writer accepts the first segment");
    passed &= contract_expect(session.value()->write(std::span(payload).subspan(5), {}).has_value(),
                              "staged writer accepts the final segment");
    passed &= contract_expect(!storage.reader.get_attributes("catalog/contract/base", {}),
                              "destination is invisible before staged completion");
    passed &= contract_expect(session.value()->complete({}).has_value(),
                              "staged writer completes explicitly");
    passed &= contract_expect(!storage.reader.get_attributes("catalog/contract/base", {}),
                              "destination remains invisible until publish");

    auto published = storage.publisher.publish({"staging/contract/base", "catalog/contract/base",
                                                ports::PublishCondition::kCreateOnly, std::nullopt},
                                               {});
    passed &= contract_expect(published && published.value().size_bytes == payload.size() &&
                                  !published.value().generation.empty(),
                              "publish creates visible attributes and generation");
    if (!published) {
        return false;
    }
    std::vector<std::byte> destination(payload.size());
    auto read = storage.reader.read_range("catalog/contract/base", 0, destination, {});
    passed &= contract_expect(read && read.value() > 0 && read.value() <= payload.size(),
                              "range reader returns a bounded non-empty read");
    if (read) {
        passed &= contract_expect(std::ranges::equal(std::span(destination).first(read.value()),
                                                     std::span(payload).first(read.value())),
                                  "range reader returns object bytes from the requested offset");
    }
    return passed;
}

[[nodiscard]] inline bool test_abort_and_conditions(ObjectStorageContractPorts& storage) {
    const auto payload = contract_bytes("replacement");
    auto aborted = storage.staged_writer.begin_staged_write("staging/contract/aborted", {});
    bool passed = contract_expect(aborted.has_value(), "storage begins an abortable write");
    if (aborted) {
        passed &= contract_expect(aborted.value()->write(payload, {}).has_value(),
                                  "abortable staged write accepts bytes");
        aborted.value()->abort();
        auto publish_aborted =
            storage.publisher.publish({"staging/contract/aborted", "catalog/contract/aborted",
                                       ports::PublishCondition::kCreateOnly, std::nullopt},
                                      {});
        passed &= contract_expect(!publish_aborted, "aborted staged object cannot be published");
    }

    auto current = storage.reader.get_attributes("catalog/contract/base", {});
    if (!current) {
        return false;
    }
    auto staged = storage.staged_writer.begin_staged_write("staging/contract/replacement", {});
    if (!staged) {
        return false;
    }
    passed &= contract_expect(staged.value()->write(payload, {}).has_value() &&
                                  staged.value()->complete({}).has_value(),
                              "replacement object is staged completely");
    auto create_conflict =
        storage.publisher.publish({"staging/contract/replacement", "catalog/contract/base",
                                   ports::PublishCondition::kCreateOnly, std::nullopt},
                                  {});
    passed &= contract_expect(!create_conflict &&
                                  create_conflict.error().code == base::ErrorCode::kConflict,
                              "create-only publish rejects an existing destination");
    auto generation_conflict = storage.publisher.publish(
        {"staging/contract/replacement", "catalog/contract/base",
         ports::PublishCondition::kReplaceIfGenerationMatches, "wrong-generation"},
        {});
    passed &= contract_expect(!generation_conflict &&
                                  generation_conflict.error().code == base::ErrorCode::kConflict,
                              "conditional replace rejects a stale generation");
    auto replaced = storage.publisher.publish(
        {"staging/contract/replacement", "catalog/contract/base",
         ports::PublishCondition::kReplaceIfGenerationMatches, current.value().generation},
        {});
    passed &= contract_expect(replaced && replaced.value().generation != current.value().generation,
                              "conditional replace publishes a new generation");
    return passed;
}

[[nodiscard]] inline bool test_pagination(ObjectStorageContractPorts& storage) {
    const auto payload = contract_bytes("page");
    auto first = stage_and_publish(storage, "staging/contract/page-a", "catalog/pages/a", payload);
    auto second = stage_and_publish(storage, "staging/contract/page-b", "catalog/pages/b", payload);
    auto third = stage_and_publish(storage, "staging/contract/page-c", "catalog/pages/c", payload);
    bool passed = contract_expect(first && second && third, "pagination fixtures publish");
    if (!passed) {
        return false;
    }
    auto first_page = storage.enumerator.enumerate({"catalog/pages/", std::nullopt, 2}, {});
    passed &= contract_expect(first_page && first_page.value().objects.size() == 2 &&
                                  first_page.value().continuation_token.has_value(),
                              "prefix enumeration returns a bounded first page");
    if (!first_page || !first_page.value().continuation_token) {
        return false;
    }
    auto final_page = storage.enumerator.enumerate(
        {"catalog/pages/", first_page.value().continuation_token, 2}, {});
    passed &= contract_expect(final_page && final_page.value().objects.size() == 1 &&
                                  !final_page.value().continuation_token,
                              "prefix enumeration resumes with an opaque token");
    passed &=
        contract_expect(final_page && final_page.value().objects.front().key == "catalog/pages/c",
                        "prefix enumeration is stable and duplicate-free");
    return passed;
}

[[nodiscard]] inline bool test_delete_contract(ObjectStorageContractPorts& storage) {
    auto current = storage.reader.get_attributes("catalog/contract/base", {});
    if (!current) {
        return false;
    }
    auto stale = storage.deleter.delete_object(
        {"catalog/contract/base", "delete-contract", "stale-generation"}, {});
    bool passed = contract_expect(!stale && stale.error().code == base::ErrorCode::kConflict,
                                  "delete rejects a stale generation");
    auto removed = storage.deleter.delete_object(
        {"catalog/contract/base", "delete-contract", current.value().generation}, {});
    passed &= contract_expect(removed.has_value(), "delete accepts the observed generation");
    passed &= contract_expect(!storage.reader.get_attributes("catalog/contract/base", {}),
                              "deleted object is no longer visible");
    passed &= contract_expect(
        storage.deleter
            .delete_object({"catalog/contract/base", "delete-contract", current.value().generation},
                           {})
            .has_value(),
        "same delete operation retries idempotently");
    passed &= contract_expect(
        storage.deleter
            .delete_object({"catalog/contract/missing", "delete-contract", std::nullopt}, {})
            .has_value(),
        "one operation ID can delete multiple objects and missing objects succeed");
    auto changed_retry = storage.deleter.delete_object(
        {"catalog/contract/base", "delete-contract", "changed-generation"}, {});
    passed &=
        contract_expect(!changed_retry && changed_retry.error().code == base::ErrorCode::kConflict,
                        "delete retry cannot change its expected generation");
    return passed;
}

[[nodiscard]] inline bool test_concurrent_reads(ObjectStorageContractPorts& storage) {
    std::atomic_bool succeeded{true};
    std::vector<std::jthread> readers;
    for (std::size_t worker = 0; worker < 4; ++worker) {
        readers.emplace_back([&storage, &succeeded] {
            std::array<std::byte, 4> destination{};
            for (std::size_t iteration = 0; iteration < 32; ++iteration) {
                auto attributes = storage.reader.get_attributes("catalog/pages/a", {});
                auto read = storage.reader.read_range("catalog/pages/a", 0, destination, {});
                if (!attributes || !read || read.value() == 0) {
                    succeeded.store(false);
                    return;
                }
            }
        });
    }
    for (auto& reader : readers) {
        reader.join();
    }
    return contract_expect(succeeded.load(), "capability objects support concurrent reads");
}

[[nodiscard]] inline bool test_cancellation(ObjectStorageContractPorts& storage) {
    const auto payload = contract_bytes("cancelled-publish");
    auto write_session =
        storage.staged_writer.begin_staged_write("staging/contract/cancelled-write", {});
    auto complete_session =
        storage.staged_writer.begin_staged_write("staging/contract/cancelled-complete", {});
    auto publish_session =
        storage.staged_writer.begin_staged_write("staging/contract/cancelled-publish", {});
    if (!write_session || !complete_session || !publish_session ||
        !complete_session.value()->write(payload, {}) ||
        !publish_session.value()->write(payload, {}) || !publish_session.value()->complete({})) {
        return false;
    }
    base::CancellationSource source;
    source.request_stop();
    const auto token = source.get_token();
    std::byte destination{};
    bool passed = contract_expect(!storage.reader.get_attributes("catalog/pages/a", token),
                                  "attribute reads observe cancellation");
    passed &=
        contract_expect(!storage.reader.read_range("catalog/pages/a", 0, {&destination, 1}, token),
                        "range reads observe cancellation");
    passed &= contract_expect(
        !storage.staged_writer.begin_staged_write("staging/contract/cancelled", token),
        "staged writes observe cancellation");
    passed &= contract_expect(!write_session.value()->write(payload, token),
                              "staged session writes observe cancellation");
    passed &= contract_expect(!complete_session.value()->complete(token),
                              "staged session completion observes cancellation");
    write_session.value()->abort();
    complete_session.value()->abort();
    passed &=
        contract_expect(!storage.enumerator.enumerate({"catalog/pages/", std::nullopt, 1}, token),
                        "prefix enumeration observes cancellation");
    passed &= contract_expect(
        !storage.publisher.publish({"staging/contract/cancelled-publish",
                                    "catalog/contract/cancelled-publish",
                                    ports::PublishCondition::kCreateOnly, std::nullopt},
                                   token),
        "publish observes cancellation");
    passed &=
        contract_expect(!storage.reader.get_attributes("catalog/contract/cancelled-publish", {}),
                        "cancelled publish leaves its destination absent");
    passed &= contract_expect(!storage.deleter.delete_object(
                                  {"catalog/pages/a", "cancelled-delete", std::nullopt}, token),
                              "delete observes cancellation");
    return passed;
}

[[nodiscard]] inline bool run_object_storage_contract(ObjectStorageContractPorts storage) {
    const auto capabilities = storage.capability_provider.capabilities();
    bool passed = contract_expect(capabilities.conditional_create,
                                  "contract adapter declares conditional create support");
    passed &= test_stage_publish_and_read(storage);
    passed &= test_abort_and_conditions(storage);
    passed &= test_pagination(storage);
    passed &= test_concurrent_reads(storage);
    passed &= test_delete_contract(storage);
    passed &= test_cancellation(storage);
    return passed;
}

} // namespace aegra::tests
