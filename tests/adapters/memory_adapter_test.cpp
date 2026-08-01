#include "aegra/adapters/memory/memory_backup_store.h"
#include "aegra/adapters/memory/memory_block_io.h"

#include <array>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <span>
#include <vector>

namespace {

using aegra::adapters::memory::MemoryBackupStore;
using aegra::adapters::memory::MemoryBackupStoreOptions;
using aegra::adapters::memory::MemoryBlockSink;
using aegra::adapters::memory::MemoryBlockSource;
using aegra::adapters::memory::MemoryBlockSourceOptions;

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

std::vector<std::byte> bytes(const std::size_t size) {
    std::vector<std::byte> result(size);
    for (std::size_t index = 0; index < size; ++index) {
        result[index] = static_cast<std::byte>(index % 251);
    }
    return result;
}

bool source_contract() {
    const auto input = bytes(16);
    MemoryBlockSource source(input, MemoryBlockSourceOptions{3, std::nullopt});
    std::array<std::byte, 8> destination{};
    auto read = source.read(2, destination, {});

    bool passed = expect(read && read.value() == 3, "source supports deterministic short reads");
    passed &= expect(destination[0] == input[2] && destination[2] == input[4],
                     "source copies bytes at the requested offset");
    auto out_of_range = source.read(17, destination, {});
    passed &= expect(!out_of_range &&
                         out_of_range.error().code == aegra::base::ErrorCode::kInvalidArgument,
                     "source rejects offsets beyond EOF");

    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    auto cancelled = source.read(0, destination, cancellation.get_token());
    passed &= expect(!cancelled && cancelled.error().code == aegra::base::ErrorCode::kCancelled,
                     "source observes cancellation");

    MemoryBlockSource failing(input, MemoryBlockSourceOptions{8, 5});
    auto before_failure = failing.read(4, destination, {});
    auto at_failure = failing.read(5, destination, {});
    passed &= expect(before_failure && before_failure.value() == 1,
                     "source stops a read at the injected failure boundary");
    passed &= expect(!at_failure && at_failure.error().code == aegra::base::ErrorCode::kIoFailure,
                     "source reports an injected read failure");
    return passed;
}

bool sink_contract() {
    MemoryBlockSink sink(8);
    const auto input = bytes(4);
    auto write = sink.write(2, input, {});
    bool passed = expect(write.has_value(), "sink accepts an in-range full write");
    const auto snapshot = sink.snapshot();
    passed &= expect(snapshot[2] == input[0] && snapshot[5] == input[3],
                     "sink stores bytes at the requested offset");

    auto too_large = sink.write(6, input, {});
    passed &=
        expect(!too_large && too_large.error().code == aegra::base::ErrorCode::kInsufficientSpace,
               "sink rejects writes beyond capacity");
    passed &= expect(sink.flush({}).has_value() && sink.flush_count() == 1,
                     "sink records a successful flush");

    aegra::adapters::memory::MemoryBlockSinkOptions options;
    options.fail_at_offset = 3;
    MemoryBlockSink failing(8, options);
    auto failure = failing.write(2, input, {});
    passed &= expect(!failure && failure.error().code == aegra::base::ErrorCode::kIoFailure,
                     "sink reports an injected write failure");
    return passed;
}

aegra::ports::ChunkWriteRequest chunk_request(const std::uint64_t index, const std::uint64_t offset,
                                              const std::span<const std::byte> payload) {
    return aegra::ports::ChunkWriteRequest{
        {index, offset, payload.size(), payload.size()},
        payload,
    };
}

bool backup_store_contract() {
    const auto input = bytes(6);
    MemoryBackupStore store;
    auto session_result = store.create_session(input.size());
    bool passed = expect(session_result.has_value(), "store creates one write session");
    if (!session_result) {
        return false;
    }
    auto& session = *session_result.value();
    passed &=
        expect(session.write_chunk(chunk_request(0, 0, std::span(input).first(2)), {}).has_value(),
               "session accepts first contiguous chunk");
    passed &= expect(
        session.write_chunk(chunk_request(1, 2, std::span(input).subspan(2)), {}).has_value(),
        "session accepts final contiguous chunk");
    passed &= expect(session.commit({}).has_value() && store.is_committed(),
                     "session publishes only after commit");

    auto reader_result = store.open_reader();
    passed &= expect(reader_result.has_value(), "committed store opens a reader");
    if (reader_result) {
        auto& reader = *reader_result.value();
        auto second = reader.read_chunk(1, {});
        passed &= expect(reader.chunk_count() == 2 && second.has_value(),
                         "reader exposes committed chunks");
        passed &= expect(second && second.value().payload.size() == 4,
                         "reader returns an owned payload copy");
    }

    MemoryBackupStore aborted;
    auto aborted_session = aborted.create_session(1);
    aborted_session.value()->abort();
    passed &= expect(aborted.is_aborted() && !aborted.open_reader(),
                     "aborted data is not visible to readers");

    MemoryBackupStore failing(MemoryBackupStoreOptions{0, false});
    auto failing_session = failing.create_session(input.size());
    auto failed_write = failing_session.value()->write_chunk(chunk_request(0, 0, input), {});
    passed &=
        expect(!failed_write && failed_write.error().code == aegra::base::ErrorCode::kIoFailure,
               "store supports deterministic write failure injection");
    failing_session.value()->abort();
    return passed;
}

int run_tests() {
    const bool passed = source_contract() && sink_contract() && backup_store_contract();
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
