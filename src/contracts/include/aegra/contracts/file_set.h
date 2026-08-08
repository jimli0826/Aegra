#pragma once

#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace aegra::contracts {

// Product / format hard limits (F0). Configurations may only tighten these values.
inline constexpr std::uint32_t kMaximumFileSelections = 100;
inline constexpr std::uint32_t kMaximumFileDirectoryDepth = 64;
inline constexpr std::uint32_t kMaximumRelativePathComponents = 64;
inline constexpr std::uint32_t kMaximumNameUtf16LeBytes = 512;
inline constexpr std::uint32_t kMinimumNameUtf16LeBytes = 2;
inline constexpr std::uint32_t kMaximumAdsPerEntry = 16;
inline constexpr std::uint32_t kMaximumPlatformMetadataBytes = 64U * 1024U;
/// FileEntryDesc.flags (V7 ENTRY_FLAG_*).
inline constexpr std::uint32_t kEntryFlagHasSecurity = 0x0001U;
inline constexpr std::uint32_t kEntryFlagSparseMain = 0x0002U;
inline constexpr std::uint32_t kEntryFlagCaseSensitive = 0x0004U;
inline constexpr std::uint32_t kMaximumFileRestoreEntryIds = 10'000;
/// Distinct stable error codes retained on PartialRestoreStats (wire / TaskResult).
inline constexpr std::size_t kMaximumPartialRestoreErrorCodes = 64;
inline constexpr std::uint32_t kMaximumDisplayLabelBytes = 256;
inline constexpr std::uint32_t kMaximumNodeTokenBytes = 512;
/// Service→Worker target root encoding (volume identity + relative path blob).
inline constexpr std::uint32_t kMaximumTargetRootIdentityBytes = 4'096;
inline constexpr std::uint32_t kMaximumPreflightTokenBytes = 1'024;
inline constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

enum class ContentKind : std::uint8_t {
    kVolumeSet = 1,
    kFileSet = 2,
};

enum class NameEncoding : std::uint8_t {
    kWindowsUtf16Le = 1,
};

enum class FileEntryKind : std::uint8_t {
    kDirectory = 1,
    kFile = 2,
    kReparse = 3,
    kOther = 4,
};

enum class FileRecursion : std::uint8_t {
    kSelfOnly = 1,
    kRecursive = 2,
};

enum class FileReparsePolicy : std::uint8_t {
    kCaptureNoFollow = 1,
};

enum class FileUnreadablePolicy : std::uint8_t {
    kFailJob = 1,
};

enum class FileConflictPolicy : std::uint8_t {
    kFail = 1,
    kReplace = 2,
    kRename = 3,
};

enum class FileNodeSelectability : std::uint8_t {
    kSelectable = 1,
    kNotSelectable = 2,
    kUnsupported = 3,
};

enum class FileStreamKind : std::uint8_t {
    kMain = 1,
    kAlternate = 2,
};

/// One normalized path component. Bytes are raw UTF-16LE code units (even length).
/// Contracts never convert lossily to UTF-8.
struct EncodedName final {
    NameEncoding encoding{NameEncoding::kWindowsUtf16Le};
    std::vector<std::byte> bytes;
};

/// Trusted Worker File Source Ref (Service-to-Worker only).
/// relative_components are normalized; never an absolute path string.
struct FileSourceRef final {
    std::string selection_id;
    std::string volume_identity;
    std::vector<EncodedName> relative_components;
    FileEntryKind entry_kind{FileEntryKind::kDirectory};
    FileRecursion recursion{FileRecursion::kRecursive};
    FileReparsePolicy reparse_policy{FileReparsePolicy::kCaptureNoFollow};
    FileUnreadablePolicy unreadable_policy{FileUnreadablePolicy::kFailJob};
    std::string display_label;
};

struct FileStreamExtentDesc final {
    std::uint64_t chunk_index{0};
    std::uint32_t block_entry_index{0};
    std::uint64_t file_offset{0};
    std::uint64_t logical_size{0};
};

struct FileAllocatedRangeDesc final {
    std::uint64_t offset{0};
    std::uint64_t length{0};
};

struct FileStreamDesc final {
    std::uint32_t stream_index{0};
    FileStreamKind stream_kind{FileStreamKind::kMain};
    EncodedName name;
    std::uint64_t logical_size{0};
    std::vector<FileStreamExtentDesc> extents;
    std::vector<FileAllocatedRangeDesc> allocated_ranges;
};

/// Portable entry metadata shared by Ports and format adapters (no Win32 types).
struct FileEntryDesc final {
    std::uint64_t entry_id{0};
    std::uint64_t parent_entry_id{0};
    FileEntryKind kind{FileEntryKind::kFile};
    EncodedName name;
    std::string selection_id;
    std::uint32_t attributes{0};
    std::uint32_t flags{0};
    std::uint64_t creation_time{0};
    std::uint64_t access_time{0};
    std::uint64_t write_time{0};
    std::uint64_t change_time{0};
    std::uint64_t logical_size{0};
    std::uint64_t hard_link_group{0};
    std::vector<FileStreamDesc> streams;
    std::vector<std::byte> platform_metadata;
};

struct FileRestoreTarget final {
    /// Service-resolved target root identity (opaque to Desktop; Worker revalidates).
    std::string target_root_identity;
    /// Decimal u64 entry IDs selected for restore (1..kMaximumFileRestoreEntryIds).
    std::vector<std::string> entry_ids;
    FileConflictPolicy conflict_policy{FileConflictPolicy::kFail};
    bool restore_security{true};
    bool restore_ads{true};
};

struct PartialRestoreStats final {
    std::uint64_t entries_requested{0};
    std::uint64_t entries_restored{0};
    std::uint64_t entries_failed{0};
    std::uint64_t bytes_restored{0};
    std::vector<std::string> stable_error_codes;
};

[[nodiscard]] bool is_known_content_kind(ContentKind kind) noexcept;
[[nodiscard]] bool is_known_name_encoding(NameEncoding encoding) noexcept;
[[nodiscard]] bool is_known_file_entry_kind(FileEntryKind kind) noexcept;
[[nodiscard]] bool is_known_file_recursion(FileRecursion recursion) noexcept;
[[nodiscard]] bool is_known_file_conflict_policy(FileConflictPolicy policy) noexcept;

[[nodiscard]] base::Result<void> validate_encoded_name(const EncodedName& name);
[[nodiscard]] base::Result<void> validate_file_source_ref(const FileSourceRef& ref);
[[nodiscard]] base::Result<void>
validate_file_source_refs(const std::vector<FileSourceRef>& refs);
[[nodiscard]] base::Result<void> validate_file_restore_target(const FileRestoreTarget& target);
[[nodiscard]] base::Result<void> validate_partial_restore_stats(const PartialRestoreStats& stats);

} // namespace aegra::contracts
