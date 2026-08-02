#include "aegra/pipeline/verify_pipeline.h"

#include "aegra/base/cancellation.h"
#include "aegra/ports/backup_session.h"

#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <span>
#include <vector>

namespace {

class FakeReader final : public aegra::ports::IRecoveryPointReader {
  public:
    std::uint64_t logical_size_bytes() const noexcept override { return logical_size_; }
    std::uint64_t chunk_count() const noexcept override { return descriptors_.size(); }

    aegra::base::Result<aegra::ports::ChunkDescriptor>
    describe_chunk(const std::uint64_t index) const override {
        if (index >= descriptors_.size()) {
            return aegra::base::Result<aegra::ports::ChunkDescriptor>::failure(
                {aegra::base::ErrorCode::kNotFound, "fake chunk not found"});
        }
        return aegra::base::Result<aegra::ports::ChunkDescriptor>::success(descriptors_[index]);
    }

    aegra::base::Result<aegra::ports::ChunkData>
    read_chunk(const std::uint64_t index, aegra::base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return aegra::base::Result<aegra::ports::ChunkData>::failure(
                {aegra::base::ErrorCode::kCancelled, "fake read cancelled"});
        }
        auto descriptor = describe_chunk(index);
        if (!descriptor) {
            return aegra::base::Result<aegra::ports::ChunkData>::failure(descriptor.error());
        }
        if (change_descriptor_) {
            descriptor.value().logical_size += 1;
        }
        return aegra::base::Result<aegra::ports::ChunkData>::success(
            {descriptor.value(), std::vector<std::byte>(descriptor.value().logical_size)});
    }

    std::uint64_t logical_size_{8};
    std::vector<aegra::ports::ChunkDescriptor> descriptors_{{0, 0, 4, 4}, {1, 4, 4, 4}};
    bool change_descriptor_{false};
};

bool expect(const bool condition, const char* message) {
    if (condition) {
        return true;
    }
    std::fprintf(stderr, "[FAIL] %s\n", message);
    return false;
}

bool test_success() {
    FakeReader reader;
    aegra::pipeline::VerifyPipeline pipeline(reader);
    auto result = pipeline.run({"job-1", "trace-1"}, {});
    return expect(result && result.value().logical_bytes == 8 &&
                      result.value().verified_bytes == 8 && result.value().chunk_count == 2,
                  "verify pipeline authenticates every chunk");
}

bool test_rejects_descriptor_change() {
    FakeReader reader;
    reader.change_descriptor_ = true;
    aegra::pipeline::VerifyPipeline pipeline(reader);
    auto result = pipeline.run({"job-1", "trace-1"}, {});
    return expect(!result && result.error().code == aegra::base::ErrorCode::kCorruptData,
                  "verify pipeline rejects changed chunk payload descriptors");
}

bool test_cancellation() {
    FakeReader reader;
    aegra::base::CancellationSource cancellation;
    cancellation.request_stop();
    aegra::pipeline::VerifyPipeline pipeline(reader);
    auto result = pipeline.run({"job-1", "trace-1"}, cancellation.get_token());
    return expect(!result && result.error().code == aegra::base::ErrorCode::kCancelled,
                  "verify pipeline observes cancellation before reading");
}

bool test_rejects_overlap() {
    FakeReader reader;
    reader.descriptors_[1].logical_offset = 3;
    aegra::pipeline::VerifyPipeline pipeline(reader);
    auto result = pipeline.run({"job-1", "trace-1"}, {});
    return expect(!result && result.error().code == aegra::base::ErrorCode::kCorruptData,
                  "verify pipeline rejects overlapping chunks");
}

} // namespace

int main() noexcept {
    try {
        return test_success() && test_rejects_descriptor_change() && test_cancellation() &&
                       test_rejects_overlap()
                   ? EXIT_SUCCESS
                   : EXIT_FAILURE;
    } catch (...) {
        std::fputs("[FAIL] unexpected exception\n", stderr);
        return EXIT_FAILURE;
    }
}
