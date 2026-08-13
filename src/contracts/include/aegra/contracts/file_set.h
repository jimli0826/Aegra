#pragma once

#include "aegra/base/result.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <vector>

namespace aegra::contracts {

// Product / format hard limits. Configurations may only tighten these values.
// Current file_set: directory / regular file / unnamed main stream; local|parent content.
inline constexpr std::uint32_t kMaximumFileSelections = 100;
inline constexpr std::uint32_t kMaximumFileDirectoryDepth = 64;
inline constexpr std::uint32_t kMaximumRelativePathComponents = 64;
inline constexpr std::uint32_t kMaximumNameUtf16LeBytes = 512;
inline constexpr std::uint32_t kMinimumNameUtf16LeBytes = 2;
inline constexpr std::uint32_t kMaximumPlatformMetadataBytes = 64U * 1024U;
/// FileEntryDesc.flags (V7 ENTRY_FLAG_*).
inline constexpr std::uint32_t kEntryFlagHasSecurity = 0x0001U;
inline constexpr std::uint32_t kEntryFlagCaseSensitive = 0x0004U;
/// Known flags mask; any other bit is corrupt/unsupported current format.
inline constexpr std::uint32_t kEntryFlagsKnownMask =
    kEntryFlagHasSecurity | kEntryFlagCaseSensitive;
inline constexpr std::uint32_t kMaximumFileRestoreEntryIds = 10'000;
/// Distinct stable error codes retained on PartialRestoreStats (wire / TaskResult).
inline constexpr std::size_t kMaximumPartialRestoreErrorCodes = 64;
inline constexpr std::uint32_t kMaximumDisplayLabelBytes = 256;
inline constexpr std::uint32_t kMaximumNodeTokenBytes = 512;
/// Service→Worker target root encoding (volume identity + relative path blob).
inline constexpr std::uint32_t kMaximumTargetRootIdentityBytes = 4'096;
inline constexpr std::uint32_t kMaximumPreflightTokenBytes = 1'024;
inline constexpr std::uint32_t kMaximumVolumeIdentityBytes = 512;
/// FILE_ID_128 / stable identity file id width.
inline constexpr std::size_t kStableFileIdBytes = 16;
/// Selection fingerprint: SHA-256 over canonical preimage (algorithm id 1).
inline constexpr std::uint8_t kSelectionFingerprintAlgorithmSha256V1 = 1;
inline constexpr std::size_t kSelectionFingerprintBytes = 32;
/// File recovery chain depth (tip inclusive).
inline constexpr std::uint32_t kMaximumFileChainDepth = 128;
inline constexpr std::uint64_t kMaximumWireInteger =
    static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)());

enum class ContentKind : std::uint8_t {
    kVolumeSet = 1,
    kFileSet = 2,
};

enum class NameEncoding : std::uint8_t {
    kWindowsUtf16Le = 1,
};

/// Current format supports directory and regular file only.
enum class FileEntryKind : std::uint8_t {
    kDirectory = 1,
    kFile = 2,
};

enum class FileRecursion : std::uint8_t {
    kSelfOnly = 1,
    kRecursive = 2,
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

/// Current format: one unnamed main stream per regular file.
enum class FileStreamKind : std::uint8_t {
    kMain = 1,
};

/// Where stream payload bytes live for this Recovery Point layer.
enum class FileContentStorage : std::uint8_t {
    kLocal = 1,
    kParent = 2,
};

/// Persisted file_set content-change detection method.
enum class FileChangeDetectionMethod : std::uint8_t {
    kNone = 0,
    kMtimeSizeV1 = 1,
};

/// Non-sensitive reason Incremental request became effective Full.
enum class IncrementalDowngradeReason : std::uint8_t {
    kNone = 0,
    kNoParent = 1,
    kSelectionChanged = 2,
    kChainIncomplete = 3,
    kBaselineInvalid = 9,
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
    FileUnreadablePolicy unreadable_policy{FileUnreadablePolicy::kFailJob};
    std::string display_label;
};

/// Stable identity within one volume: (volume_identity, FILE_ID_128).
/// Null identity: empty volume_identity and all-zero file_id (allowed when the source filesystem
/// does not provide a stable file identity, such as FAT32).
struct StableFileIdentity final {
    std::string volume_identity;
    std::array<std::byte, kStableFileIdBytes> file_id{};

    [[nodiscard]] friend bool operator==(const StableFileIdentity&,
                                         const StableFileIdentity&) = default;
};

/// Authenticated selection fingerprint (algorithm + fixed digest).
struct FileSelectionFingerprint final {
    std::uint8_t algorithm_id{kSelectionFingerprintAlgorithmSha256V1};
    std::array<std::byte, kSelectionFingerprintBytes> digest{};

    [[nodiscard]] friend bool operator==(const FileSelectionFingerprint&,
                                         const FileSelectionFingerprint&) = default;
};

struct FileStreamExtentDesc final {
    std::uint64_t chunk_index{0};
    std::uint32_t block_entry_index{0};
    std::uint64_t file_offset{0};
    std::uint64_t logical_size{0};
};

struct FileStreamDesc final {
    std::uint32_t stream_index{0};
    FileStreamKind stream_kind{FileStreamKind::kMain};
    /// Main stream name must be empty (unnamed $DATA).
    EncodedName name;
    std::uint64_t logical_size{0};
    FileContentStorage content_storage{FileContentStorage::kLocal};
    /// Local only: extents cover logical_size. Parent must be empty.
    std::vector<FileStreamExtentDesc> extents;
    /// Parent only: direct parent layer stream_index. Local must be 0.
    std::uint32_t parent_stream_index{0};
};

/// Portable entry metadata shared by Ports and format adapters (no Win32 types).
struct FileEntryDesc final {
    std::uint64_t entry_id{0};
    std::uint64_t parent_entry_id{0};
    FileEntryKind kind{FileEntryKind::kFile};
    EncodedName name;
    std::string selection_id;
    /// Null when the source filesystem does not provide a stable file identity.
    StableFileIdentity stable_identity;
    std::uint32_t attributes{0};
    std::uint32_t flags{0};
    std::uint64_t creation_time{0};
    std::uint64_t access_time{0};
    std::uint64_t write_time{0};
    std::uint64_t change_time{0};
    std::uint64_t logical_size{0};
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
[[nodiscard]] bool is_known_file_stream_kind(FileStreamKind kind) noexcept;
[[nodiscard]] bool is_known_file_content_storage(FileContentStorage storage) noexcept;
[[nodiscard]] bool is_known_file_change_detection_method(FileChangeDetectionMethod method) noexcept;
[[nodiscard]] bool
is_known_incremental_downgrade_reason(IncrementalDowngradeReason reason) noexcept;
[[nodiscard]] bool is_known_selection_fingerprint_algorithm(std::uint8_t algorithm_id) noexcept;

[[nodiscard]] bool is_null_stable_file_identity(const StableFileIdentity& identity) noexcept;
[[nodiscard]] bool
is_zero_selection_fingerprint(const FileSelectionFingerprint& fingerprint) noexcept;

[[nodiscard]] base::Result<void> validate_encoded_name(const EncodedName& name);
[[nodiscard]] base::Result<void> validate_stable_file_identity(const StableFileIdentity& identity,
                                                               bool allow_null);
[[nodiscard]] base::Result<void>
validate_file_selection_fingerprint(const FileSelectionFingerprint& fingerprint);
[[nodiscard]] base::Result<void> validate_file_source_ref(const FileSourceRef& ref);
[[nodiscard]] base::Result<void> validate_file_source_refs(const std::vector<FileSourceRef>& refs);
/// UI-only label for one relative component (UTF-16LE → UTF-8, C0 stripped). Not a path.
[[nodiscard]] std::string file_component_display_label(const EncodedName& name);
/// Fixed English browse-root labels (Desktop, Downloads, Documents, Pictures, Music, Videos).
[[nodiscard]] bool is_file_special_folder_display_label(std::string_view label) noexcept;
/// Browse-tree display names for edit rehydrate. Special-folder product roots use their short
/// label; other selections use volume-relative component labels. Whole-volume uses display_label.
[[nodiscard]] std::vector<std::string> file_selection_display_chain(const FileSourceRef& ref);
[[nodiscard]] base::Result<void> validate_file_stream_desc(const FileStreamDesc& stream);
[[nodiscard]] base::Result<void> validate_file_entry_desc(const FileEntryDesc& entry);
[[nodiscard]] base::Result<void> validate_file_restore_target(const FileRestoreTarget& target);
[[nodiscard]] base::Result<void> validate_partial_restore_stats(const PartialRestoreStats& stats);

/// Canonical preimage for selection fingerprint algorithm 1 (SHA-256 input).
/// Caller hashes with SHA-256; contracts stay free of crypto libraries.
[[nodiscard]] base::Result<std::vector<std::byte>>
encode_file_selection_fingerprint_preimage(const std::vector<FileSourceRef>& refs);

} // namespace aegra::contracts
