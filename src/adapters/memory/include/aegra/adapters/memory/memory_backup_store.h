#pragma once

#include "aegra/ports/backup_session.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace aegra::adapters::memory {
namespace detail {
struct MemoryBackupState;
}

struct MemoryBackupStoreOptions final {
    std::optional<std::uint64_t> fail_write_chunk_index;
    bool fail_commit{false};
};

class MemoryBackupStore final {
  public:
    explicit MemoryBackupStore(MemoryBackupStoreOptions options = {});
    ~MemoryBackupStore();

    MemoryBackupStore(const MemoryBackupStore&) = delete;
    MemoryBackupStore& operator=(const MemoryBackupStore&) = delete;
    MemoryBackupStore(MemoryBackupStore&&) noexcept;
    MemoryBackupStore& operator=(MemoryBackupStore&&) noexcept;

    [[nodiscard]] base::Result<std::unique_ptr<ports::IBackupSession>>
    create_session(std::uint64_t logical_size_bytes);
    [[nodiscard]] base::Result<std::unique_ptr<ports::IRecoveryPointReader>> open_reader() const;

    [[nodiscard]] bool is_committed() const;
    [[nodiscard]] bool is_aborted() const;

  private:
    std::shared_ptr<detail::MemoryBackupState> state_;
};

} // namespace aegra::adapters::memory
