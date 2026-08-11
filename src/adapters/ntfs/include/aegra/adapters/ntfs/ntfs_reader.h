#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ports/random_access.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace aegra::adapters::ntfs {

// Product limits (ADR-0023). Configurations may only tighten these values.
inline constexpr std::uint32_t kMaximumDirectoryPage = 256;
inline constexpr std::uint32_t kMaximumStreamReadBytes = 1U * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumMftRecordBytes = 64U * 1024U;
inline constexpr std::uint32_t kMaximumIndexRecordBytes = 1U * 1024U * 1024U;
inline constexpr std::uint32_t kMaximumAttributeListDepth = 32;
inline constexpr std::uint32_t kMaximumNavigationDepth = 256;
inline constexpr std::uint32_t kMftRecordCacheCapacity = 1024;
inline constexpr std::uint32_t kIndexPageCacheCapacity = 256;
inline constexpr std::uint64_t kNtfsRootFileReference = 5;

/// MFT file reference: low 48 bits = record number, high 16 bits = sequence.
struct NtfsFileReference final {
    std::uint64_t record_number{0};
    std::uint16_t sequence_number{0};

    [[nodiscard]] bool operator==(const NtfsFileReference&) const noexcept = default;
};

struct NtfsVolumeInfo final {
    std::uint32_t bytes_per_sector{0};
    std::uint32_t sectors_per_cluster{0};
    std::uint32_t bytes_per_cluster{0};
    std::uint32_t bytes_per_mft_record{0};
    std::uint32_t bytes_per_index_record{0};
    std::uint64_t total_clusters{0};
    std::uint64_t volume_size_bytes{0};
    std::uint64_t mft_start_cluster{0};
    std::uint64_t volume_serial{0};
};

struct NtfsDirectoryContinuation final {
    /// Opaque, self-contained token for the next page. Empty means start.
    std::string token;
};

struct NtfsEntry final {
    NtfsFileReference reference{};
    /// UTF-16LE code units (no conversion to UTF-8).
    std::u16string name;
    bool is_directory{false};
    bool is_reparse{false};
    bool is_compressed{false};
    bool is_encrypted{false};
    bool is_hidden{false};
    bool is_system{false};
    std::uint32_t file_attributes{0};
    std::uint64_t logical_size{0};
    std::uint64_t allocated_size{0};
    std::uint64_t creation_time{0};     // NTFS FILETIME
    std::uint64_t modification_time{0};
    std::uint64_t mft_change_time{0};
    std::uint64_t access_time{0};
};

struct NtfsDirectoryPage final {
    std::vector<NtfsEntry> items;
    std::optional<std::string> continuation_token;
};

/// Read-only NTFS volume view over an IRandomAccessReader (offset 0 = Boot Sector).
/// Not thread-safe. Caller must serialize access. The reader must outlive this object.
class NtfsVolumeReader final {
  public:
    ~NtfsVolumeReader();
    NtfsVolumeReader(const NtfsVolumeReader&) = delete;
    NtfsVolumeReader& operator=(const NtfsVolumeReader&) = delete;
    NtfsVolumeReader(NtfsVolumeReader&&) = delete;
    NtfsVolumeReader& operator=(NtfsVolumeReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<NtfsVolumeReader>>
    open(ports::IRandomAccessReader& reader, base::CancellationToken cancellation);

    [[nodiscard]] const NtfsVolumeInfo& volume_info() const noexcept;

    [[nodiscard]] base::Result<NtfsDirectoryPage>
    list_directory(NtfsFileReference directory, std::uint32_t maximum_results,
                   const std::optional<std::string>& continuation,
                   base::CancellationToken cancellation);

    [[nodiscard]] base::Result<NtfsEntry>
    describe_entry(NtfsFileReference reference, base::CancellationToken cancellation);

    /// Reads up to destination.size() bytes (caller should cap at kMaximumStreamReadBytes).
    /// Short reads only at EOF. Sparse ranges zero-fill. Compressed/EFS return stable errors.
    [[nodiscard]] base::Result<std::size_t>
    read_file(NtfsFileReference reference, std::uint64_t offset, std::span<std::byte> destination,
              base::CancellationToken cancellation);

  private:
    struct Impl;
    explicit NtfsVolumeReader(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace aegra::adapters::ntfs
