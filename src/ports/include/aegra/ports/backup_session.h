#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace aegra::ports {

struct ChunkDescriptor final {
    std::uint64_t chunk_index{0};
    std::uint64_t logical_offset{0};
    std::uint64_t logical_size{0};
    std::uint64_t stored_size{0};

    [[nodiscard]] bool operator==(const ChunkDescriptor&) const noexcept = default;
};

struct ChunkWriteRequest final {
    ChunkDescriptor descriptor;
    std::span<const std::byte> payload;
};

struct ChunkData final {
    ChunkDescriptor descriptor;
    std::vector<std::byte> payload;
};

class IBackupSession {
  public:
    IBackupSession() = default;
    virtual ~IBackupSession() = default;
    IBackupSession(const IBackupSession&) = delete;
    IBackupSession& operator=(const IBackupSession&) = delete;
    IBackupSession(IBackupSession&&) = delete;
    IBackupSession& operator=(IBackupSession&&) = delete;

    // The implementation must consume or copy payload before this call returns.
    [[nodiscard]] virtual base::Result<void> write_chunk(const ChunkWriteRequest& request,
                                                         base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> commit(base::CancellationToken cancellation) = 0;
    virtual void abort() noexcept = 0;
};

class IRecoveryPointReader {
  public:
    IRecoveryPointReader() = default;
    virtual ~IRecoveryPointReader() = default;
    IRecoveryPointReader(const IRecoveryPointReader&) = delete;
    IRecoveryPointReader& operator=(const IRecoveryPointReader&) = delete;
    IRecoveryPointReader(IRecoveryPointReader&&) = delete;
    IRecoveryPointReader& operator=(IRecoveryPointReader&&) = delete;

    // Size and descriptors remain stable for the reader lifetime.
    [[nodiscard]] virtual std::uint64_t logical_size_bytes() const noexcept = 0;
    [[nodiscard]] virtual std::uint64_t chunk_count() const noexcept = 0;
    [[nodiscard]] virtual base::Result<ChunkDescriptor>
    describe_chunk(std::uint64_t chunk_index) const = 0;
    [[nodiscard]] virtual base::Result<ChunkData>
    read_chunk(std::uint64_t chunk_index, base::CancellationToken cancellation) = 0;
};

} // namespace aegra::ports
