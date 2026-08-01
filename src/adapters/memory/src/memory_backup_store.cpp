#include "aegra/adapters/memory/memory_backup_store.h"

#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace aegra::adapters::memory {
namespace detail {

enum class StoreStatus {
    kEmpty,
    kWriting,
    kCommitted,
    kAborted,
};

struct MemoryBackupState final {
    mutable std::mutex mutex;
    MemoryBackupStoreOptions options;
    StoreStatus status{StoreStatus::kEmpty};
    std::uint64_t logical_size{0};
    std::uint64_t next_offset{0};
    std::vector<ports::ChunkData> chunks;
};

} // namespace detail
namespace {

base::Error state_error(const char* message) {
    return base::Error{base::ErrorCode::kConflict, message};
}

base::Error cancelled_error() {
    return base::Error{base::ErrorCode::kCancelled, "operation cancelled"};
}

class MemoryBackupSession final : public ports::IBackupSession {
  public:
    explicit MemoryBackupSession(std::shared_ptr<detail::MemoryBackupState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] base::Result<void> write_chunk(const ports::ChunkWriteRequest& request,
                                                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> commit(base::CancellationToken cancellation) override;
    void abort() noexcept override;

  private:
    std::shared_ptr<detail::MemoryBackupState> state_;
};

class MemoryRecoveryPointReader final : public ports::IRecoveryPointReader {
  public:
    explicit MemoryRecoveryPointReader(std::shared_ptr<detail::MemoryBackupState> state)
        : state_(std::move(state)) {}

    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept override;
    [[nodiscard]] base::Result<ports::ChunkDescriptor>
    describe_chunk(std::uint64_t chunk_index) const override;
    [[nodiscard]] base::Result<ports::ChunkData>
    read_chunk(std::uint64_t chunk_index, base::CancellationToken cancellation) override;

  private:
    std::shared_ptr<detail::MemoryBackupState> state_;
};

base::Result<void> validate_request(const detail::MemoryBackupState& state,
                                    const ports::ChunkWriteRequest& request) {
    const auto& descriptor = request.descriptor;
    if (descriptor.chunk_index != state.chunks.size() ||
        descriptor.logical_offset != state.next_offset) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunk sequence is not contiguous"});
    }
    if (descriptor.stored_size != request.payload.size()) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "stored size does not match payload"});
    }
    if (descriptor.logical_offset > state.logical_size ||
        descriptor.logical_size > state.logical_size - descriptor.logical_offset) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "chunk logical range is invalid"});
    }
    return base::Result<void>::success();
}

base::Result<void> MemoryBackupSession::write_chunk(const ports::ChunkWriteRequest& request,
                                                    const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(cancelled_error());
    }
    const std::scoped_lock lock(state_->mutex);
    if (state_->status != detail::StoreStatus::kWriting) {
        return base::Result<void>::failure(state_error("backup session is not writable"));
    }
    auto validation = validate_request(*state_, request);
    if (!validation) {
        return validation;
    }
    if (state_->options.fail_write_chunk_index == request.descriptor.chunk_index) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "injected chunk write failure"});
    }

    state_->chunks.push_back(
        ports::ChunkData{request.descriptor, {request.payload.begin(), request.payload.end()}});
    state_->next_offset += request.descriptor.logical_size;
    return base::Result<void>::success();
}

base::Result<void> MemoryBackupSession::commit(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(cancelled_error());
    }
    const std::scoped_lock lock(state_->mutex);
    if (state_->status != detail::StoreStatus::kWriting) {
        return base::Result<void>::failure(state_error("backup session is not writable"));
    }
    if (state_->next_offset != state_->logical_size) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kCorruptData, "backup does not cover logical source"});
    }
    if (state_->options.fail_commit) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kIoFailure, "injected commit failure"});
    }
    state_->status = detail::StoreStatus::kCommitted;
    return base::Result<void>::success();
}

void MemoryBackupSession::abort() noexcept {
    const std::scoped_lock lock(state_->mutex);
    if (state_->status != detail::StoreStatus::kWriting) {
        return;
    }
    state_->chunks.clear();
    state_->next_offset = 0;
    state_->status = detail::StoreStatus::kAborted;
}

std::uint64_t MemoryRecoveryPointReader::logical_size_bytes() const noexcept {
    return state_->logical_size;
}

std::uint64_t MemoryRecoveryPointReader::chunk_count() const noexcept {
    return static_cast<std::uint64_t>(state_->chunks.size());
}

base::Result<ports::ChunkDescriptor>
MemoryRecoveryPointReader::describe_chunk(const std::uint64_t chunk_index) const {
    if (chunk_index >= state_->chunks.size()) {
        return base::Result<ports::ChunkDescriptor>::failure(
            base::Error{base::ErrorCode::kNotFound, "chunk index was not found"});
    }
    return base::Result<ports::ChunkDescriptor>::success(
        state_->chunks[static_cast<std::size_t>(chunk_index)].descriptor);
}

base::Result<ports::ChunkData>
MemoryRecoveryPointReader::read_chunk(const std::uint64_t chunk_index,
                                      const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<ports::ChunkData>::failure(cancelled_error());
    }
    if (chunk_index >= state_->chunks.size()) {
        return base::Result<ports::ChunkData>::failure(
            base::Error{base::ErrorCode::kNotFound, "chunk index was not found"});
    }
    return base::Result<ports::ChunkData>::success(
        state_->chunks[static_cast<std::size_t>(chunk_index)]);
}

} // namespace

MemoryBackupStore::MemoryBackupStore(const MemoryBackupStoreOptions options)
    : state_(std::make_shared<detail::MemoryBackupState>()) {
    state_->options = options;
}

MemoryBackupStore::~MemoryBackupStore() = default;
MemoryBackupStore::MemoryBackupStore(MemoryBackupStore&&) noexcept = default;
MemoryBackupStore& MemoryBackupStore::operator=(MemoryBackupStore&&) noexcept = default;

base::Result<std::unique_ptr<ports::IBackupSession>>
MemoryBackupStore::create_session(const std::uint64_t logical_size_bytes) {
    const std::scoped_lock lock(state_->mutex);
    if (state_->status != detail::StoreStatus::kEmpty) {
        return base::Result<std::unique_ptr<ports::IBackupSession>>::failure(
            state_error("memory backup store already has a session"));
    }
    state_->logical_size = logical_size_bytes;
    state_->status = detail::StoreStatus::kWriting;
    return base::Result<std::unique_ptr<ports::IBackupSession>>::success(
        std::make_unique<MemoryBackupSession>(state_));
}

base::Result<std::unique_ptr<ports::IRecoveryPointReader>> MemoryBackupStore::open_reader() const {
    const std::scoped_lock lock(state_->mutex);
    if (state_->status != detail::StoreStatus::kCommitted) {
        return base::Result<std::unique_ptr<ports::IRecoveryPointReader>>::failure(
            state_error("memory recovery point is not committed"));
    }
    return base::Result<std::unique_ptr<ports::IRecoveryPointReader>>::success(
        std::make_unique<MemoryRecoveryPointReader>(state_));
}

bool MemoryBackupStore::is_committed() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->status == detail::StoreStatus::kCommitted;
}

bool MemoryBackupStore::is_aborted() const {
    const std::scoped_lock lock(state_->mutex);
    return state_->status == detail::StoreStatus::kAborted;
}

} // namespace aegra::adapters::memory
