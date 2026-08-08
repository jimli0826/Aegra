#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/pipeline/file_set_backup_pipeline.h"
#include "aegra/ports/progress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::apps::worker {

struct WindowsFileSetBackupRequest final {
    std::string job_id;
    std::string trace_id;
    std::vector<contracts::FileSourceRef> selections;
    std::filesystem::path destination;
    std::filesystem::path index_spool_directory;
    std::string_view password;
    bool encryption_enabled{false};
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::uint32_t block_size_bytes{0};
    std::uint32_t chunk_size_bytes{0};
    std::size_t memory_budget_bytes{0};
    std::uint64_t split_size_bytes{0};
    std::uint64_t kdf_opslimit{3};
    std::uint64_t kdf_memlimit_bytes{256ULL * 1024ULL * 1024ULL};
    std::string created_utc;
    std::string application_version;
    std::string hostname;
};

struct WindowsFileSetBackupResult final {
    pipeline::FileSetBackupSummary backup;
    std::optional<base::Error> snapshot_cleanup_error;
};

namespace detail {

[[nodiscard]] base::Result<WindowsFileSetBackupResult>
backup_windows_file_set(const WindowsFileSetBackupRequest& request,
                        const base::CancellationToken& cancellation,
                        ports::IProgressSink* progress);

} // namespace detail
} // namespace aegra::apps::worker
