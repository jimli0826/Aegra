#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/error.h"
#include "aegra/base/result.h"
#include "aegra/pipeline/backup_pipeline.h"
#include "aegra/ports/progress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aegra::apps::worker {

enum class WindowsPersonalBackupType : std::uint8_t {
    kFull = 1,
    kIncremental = 2,
};

struct WindowsPersonalVolumeBackupRequest final {
    std::string job_id;
    std::string trace_id;
    std::filesystem::path volume_guid_path;
    std::filesystem::path destination;
    std::string_view password;
    WindowsPersonalBackupType backup_type{WindowsPersonalBackupType::kFull};
    std::filesystem::path parent_source;
    std::string_view parent_password;
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

struct WindowsPersonalVolumeBackupResult final {
    pipeline::BackupSummary backup;
    std::optional<base::Error> snapshot_cleanup_error;
};

[[nodiscard]] base::Result<WindowsPersonalVolumeBackupResult>
backup_windows_personal_volume(const WindowsPersonalVolumeBackupRequest& request,
                               const base::CancellationToken& cancellation,
                               ports::IProgressSink* progress = nullptr);

} // namespace aegra::apps::worker
