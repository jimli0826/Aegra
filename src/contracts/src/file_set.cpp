#include "aegra/contracts/file_set.h"

#include "aegra/base/uuid.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <span>
#include <string_view>
#include <utility>

namespace aegra::contracts {
namespace {

base::Result<void> invalid(std::string message) {
    return base::Result<void>::failure(
        base::Error{base::ErrorCode::kInvalidArgument, std::move(message)});
}

[[nodiscard]] bool valid_wire_u64(const std::uint64_t value) noexcept {
    return value <= kMaximumWireInteger;
}

[[nodiscard]] bool valid_display_label(const std::string_view value) noexcept {
    if (value.empty() || value.size() > kMaximumDisplayLabelBytes) {
        return false;
    }
    return std::ranges::all_of(value, [](const unsigned char character) {
        return character >= 0x20U && character != 0x7FU;
    });
}

[[nodiscard]] bool valid_volume_identity(const std::string_view value) noexcept {
    return !value.empty() && value.size() <= kMaximumVolumeIdentityBytes;
}

[[nodiscard]] bool valid_relative_component_bytes(const std::vector<std::byte>& bytes) noexcept {
    if (bytes.size() < kMinimumNameUtf16LeBytes || bytes.size() > kMaximumNameUtf16LeBytes ||
        (bytes.size() % 2U) != 0U) {
        return false;
    }
    for (std::size_t index = 0; index + 1 < bytes.size(); index += 2) {
        const auto low = static_cast<std::uint8_t>(bytes[index]);
        const auto high = static_cast<std::uint8_t>(bytes[index + 1]);
        if (high != 0) {
            continue;
        }
        if (low == 0 || low == static_cast<std::uint8_t>('\\') ||
            low == static_cast<std::uint8_t>('/')) {
            return false;
        }
    }
    if (bytes.size() == 2 && static_cast<std::uint8_t>(bytes[0]) == '.' &&
        static_cast<std::uint8_t>(bytes[1]) == 0) {
        return false;
    }
    if (bytes.size() == 4 && static_cast<std::uint8_t>(bytes[0]) == '.' &&
        static_cast<std::uint8_t>(bytes[1]) == 0 && static_cast<std::uint8_t>(bytes[2]) == '.' &&
        static_cast<std::uint8_t>(bytes[3]) == 0) {
        return false;
    }
    return true;
}

[[nodiscard]] bool parse_entry_id(const std::string_view text, std::uint64_t& out) noexcept {
    if (text.empty() || text.size() > 20 || (text.size() > 1 && text[0] == '0')) {
        return false;
    }
    std::uint64_t value = 0;
    for (const char character : text) {
        if (character < '0' || character > '9') {
            return false;
        }
        const auto digit = static_cast<std::uint64_t>(character - '0');
        if (value > (kMaximumWireInteger - digit) / 10U) {
            return false;
        }
        value = value * 10U + digit;
    }
    if (value == 0) {
        return false;
    }
    out = value;
    return true;
}

[[nodiscard]] bool
file_id_is_zero(const std::array<std::byte, kStableFileIdBytes>& file_id) noexcept {
    return std::all_of(file_id.begin(), file_id.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

void append_u8(std::vector<std::byte>& out, const std::uint8_t value) {
    out.push_back(static_cast<std::byte>(value));
}

void append_u32_le(std::vector<std::byte>& out, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

void append_bytes(std::vector<std::byte>& out, const std::span<const std::byte> bytes) {
    out.insert(out.end(), bytes.begin(), bytes.end());
}

void append_string_bytes(std::vector<std::byte>& out, const std::string_view text) {
    append_bytes(out, std::span<const std::byte>(reinterpret_cast<const std::byte*>(text.data()),
                                                 text.size()));
}

[[nodiscard]] std::string path_sort_key(const FileSourceRef& ref) {
    std::string key = ref.volume_identity;
    key.push_back('\0');
    for (const auto& component : ref.relative_components) {
        key.append(reinterpret_cast<const char*>(component.bytes.data()), component.bytes.size());
        key.push_back('\0');
    }
    key.append(ref.selection_id);
    return key;
}

[[nodiscard]] base::Result<void> validate_local_stream_extents(const FileStreamDesc& stream) {
    std::uint64_t covered = 0;
    std::uint64_t next_offset = 0;
    for (const auto& extent : stream.extents) {
        if (extent.logical_size == 0 || !valid_wire_u64(extent.file_offset) ||
            !valid_wire_u64(extent.logical_size) || extent.file_offset < next_offset) {
            return invalid("file stream extents are invalid");
        }
        if (extent.logical_size > stream.logical_size - extent.file_offset) {
            return invalid("file stream extent exceeds logical_size");
        }
        next_offset = extent.file_offset + extent.logical_size;
        covered += extent.logical_size;
    }
    if (stream.logical_size == 0) {
        if (!stream.extents.empty()) {
            return invalid("empty stream must have no extents");
        }
        return base::Result<void>::success();
    }
    if (covered != stream.logical_size || next_offset != stream.logical_size) {
        return invalid("file stream extents do not cover logical_size");
    }
    return base::Result<void>::success();
}

} // namespace

bool is_known_content_kind(const ContentKind kind) noexcept {
    return kind == ContentKind::kVolumeSet || kind == ContentKind::kFileSet;
}

bool is_known_name_encoding(const NameEncoding encoding) noexcept {
    return encoding == NameEncoding::kWindowsUtf16Le;
}

bool is_known_file_entry_kind(const FileEntryKind kind) noexcept {
    return kind == FileEntryKind::kDirectory || kind == FileEntryKind::kFile;
}

bool is_known_file_recursion(const FileRecursion recursion) noexcept {
    return recursion == FileRecursion::kSelfOnly || recursion == FileRecursion::kRecursive;
}

bool is_known_file_conflict_policy(const FileConflictPolicy policy) noexcept {
    return policy == FileConflictPolicy::kFail || policy == FileConflictPolicy::kReplace ||
           policy == FileConflictPolicy::kRename;
}

bool is_known_file_stream_kind(const FileStreamKind kind) noexcept {
    return kind == FileStreamKind::kMain;
}

bool is_known_file_content_storage(const FileContentStorage storage) noexcept {
    return storage == FileContentStorage::kLocal || storage == FileContentStorage::kParent;
}

bool is_known_file_change_detection_method(const FileChangeDetectionMethod method) noexcept {
    return method == FileChangeDetectionMethod::kNone ||
           method == FileChangeDetectionMethod::kMtimeSizeV1;
}

bool is_known_incremental_downgrade_reason(const IncrementalDowngradeReason reason) noexcept {
    switch (reason) {
    case IncrementalDowngradeReason::kNone:
    case IncrementalDowngradeReason::kNoParent:
    case IncrementalDowngradeReason::kSelectionChanged:
    case IncrementalDowngradeReason::kChainIncomplete:
    case IncrementalDowngradeReason::kBaselineInvalid:
        return true;
    }
    return false;
}

bool is_known_selection_fingerprint_algorithm(const std::uint8_t algorithm_id) noexcept {
    return algorithm_id == kSelectionFingerprintAlgorithmSha256V1;
}

bool is_null_stable_file_identity(const StableFileIdentity& identity) noexcept {
    return identity.volume_identity.empty() && file_id_is_zero(identity.file_id);
}

bool is_zero_selection_fingerprint(const FileSelectionFingerprint& fingerprint) noexcept {
    return std::all_of(fingerprint.digest.begin(), fingerprint.digest.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

base::Result<void> validate_encoded_name(const EncodedName& name) {
    if (!is_known_name_encoding(name.encoding)) {
        return invalid("name encoding is invalid");
    }
    if (!valid_relative_component_bytes(name.bytes)) {
        return invalid("encoded name bytes are invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_stable_file_identity(const StableFileIdentity& identity,
                                                 const bool allow_null) {
    if (is_null_stable_file_identity(identity)) {
        return allow_null ? base::Result<void>::success()
                          : invalid("stable file identity is required");
    }
    if (!valid_volume_identity(identity.volume_identity) || file_id_is_zero(identity.file_id)) {
        return invalid("stable file identity is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void>
validate_file_selection_fingerprint(const FileSelectionFingerprint& fingerprint) {
    if (!is_known_selection_fingerprint_algorithm(fingerprint.algorithm_id) ||
        is_zero_selection_fingerprint(fingerprint)) {
        return invalid("file selection fingerprint is invalid");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_source_ref(const FileSourceRef& ref) {
    if (!base::is_canonical_uuid(ref.selection_id) || !valid_volume_identity(ref.volume_identity)) {
        return invalid("file source selection or volume identity is invalid");
    }
    // Empty relative_components means the volume root (directory only).
    if (ref.relative_components.size() > kMaximumRelativePathComponents ||
        (ref.relative_components.empty() && ref.entry_kind != FileEntryKind::kDirectory)) {
        return invalid("file source relative components count is invalid");
    }
    if (!is_known_file_entry_kind(ref.entry_kind) || !is_known_file_recursion(ref.recursion) ||
        ref.unreadable_policy != FileUnreadablePolicy::kFailJob) {
        return invalid("file source policies or kind are invalid");
    }
    if (!valid_display_label(ref.display_label)) {
        return invalid("file source display_label is invalid");
    }
    for (const auto& component : ref.relative_components) {
        auto valid = validate_encoded_name(component);
        if (!valid) {
            return valid;
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_source_refs(const std::vector<FileSourceRef>& refs) {
    if (refs.empty() || refs.size() > kMaximumFileSelections) {
        return invalid("file source ref count is invalid");
    }
    std::set<std::string_view> selection_ids;
    std::set<std::string> path_keys;
    for (const auto& ref : refs) {
        auto valid = validate_file_source_ref(ref);
        if (!valid) {
            return valid;
        }
        if (!selection_ids.insert(ref.selection_id).second) {
            return invalid("file source selection_id must be unique");
        }
        std::string key = ref.volume_identity;
        key.push_back('\0');
        for (const auto& component : ref.relative_components) {
            key.append(reinterpret_cast<const char*>(component.bytes.data()),
                       component.bytes.size());
            key.push_back('\0');
        }
        if (!path_keys.insert(std::move(key)).second) {
            return invalid("file source normalized path must be unique");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_stream_desc(const FileStreamDesc& stream) {
    if (!is_known_file_stream_kind(stream.stream_kind) || stream.stream_index == 0) {
        return invalid("file stream kind or index is invalid");
    }
    if (!is_known_name_encoding(stream.name.encoding) || !stream.name.bytes.empty()) {
        return invalid("main stream name must be empty");
    }
    if (!is_known_file_content_storage(stream.content_storage) ||
        !valid_wire_u64(stream.logical_size)) {
        return invalid("file stream content_storage or logical_size is invalid");
    }
    if (stream.content_storage == FileContentStorage::kLocal) {
        if (stream.parent_stream_index != 0) {
            return invalid("local stream cannot carry parent_stream_index");
        }
        return validate_local_stream_extents(stream);
    }
    // parent storage: no local extents; direct parent stream index required.
    if (!stream.extents.empty() || stream.parent_stream_index == 0) {
        return invalid("parent stream requires parent_stream_index and empty extents");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_entry_desc(const FileEntryDesc& entry) {
    if (entry.entry_id == 0 || !is_known_file_entry_kind(entry.kind) ||
        !base::is_canonical_uuid(entry.selection_id)) {
        return invalid("file entry id or kind is invalid");
    }
    auto name = validate_encoded_name(entry.name);
    if (!name) {
        return name;
    }
    auto identity = validate_stable_file_identity(entry.stable_identity, true);
    if (!identity) {
        return identity;
    }
    // Portable Windows attribute bits that imply unsupported object types (FI0).
    constexpr std::uint32_t kAttributeSparseFile = 0x00000200U;
    constexpr std::uint32_t kAttributeReparsePoint = 0x00000400U;
    if ((entry.attributes & (kAttributeSparseFile | kAttributeReparsePoint)) != 0) {
        return invalid("file entry attributes imply unsupported object type");
    }
    if ((entry.flags & ~kEntryFlagsKnownMask) != 0) {
        return invalid("file entry flags contain unsupported bits");
    }
    if (entry.platform_metadata.size() > kMaximumPlatformMetadataBytes) {
        return invalid("file_source.metadata_limit");
    }
    if (entry.kind == FileEntryKind::kDirectory) {
        if (entry.logical_size != 0 || !entry.streams.empty()) {
            return invalid("directory entry must not carry streams or logical_size");
        }
        return base::Result<void>::success();
    }
    if (entry.streams.size() != 1) {
        return invalid("regular file must have exactly one main stream");
    }
    auto stream = validate_file_stream_desc(entry.streams.front());
    if (!stream) {
        return stream;
    }
    if (entry.logical_size != entry.streams.front().logical_size) {
        return invalid("file logical_size must match main stream");
    }
    return base::Result<void>::success();
}

base::Result<void> validate_file_restore_target(const FileRestoreTarget& target) {
    if (target.target_root_identity.empty() ||
        target.target_root_identity.size() > kMaximumTargetRootIdentityBytes) {
        return invalid("file restore target_root_identity is invalid");
    }
    if (target.entry_ids.empty() || target.entry_ids.size() > kMaximumFileRestoreEntryIds) {
        return invalid("file restore entry_ids count is invalid");
    }
    if (!is_known_file_conflict_policy(target.conflict_policy) || !target.restore_security) {
        return invalid("file restore policy is invalid");
    }
    std::set<std::uint64_t> seen;
    for (const auto& text : target.entry_ids) {
        std::uint64_t entry_id = 0;
        if (!parse_entry_id(text, entry_id) || !seen.insert(entry_id).second) {
            return invalid("file restore entry_ids are invalid or duplicated");
        }
    }
    return base::Result<void>::success();
}

base::Result<void> validate_partial_restore_stats(const PartialRestoreStats& stats) {
    if (stats.entries_requested == 0 || stats.entries_restored > stats.entries_requested ||
        stats.entries_failed > stats.entries_requested ||
        stats.entries_restored + stats.entries_failed > stats.entries_requested ||
        !valid_wire_u64(stats.entries_requested) || !valid_wire_u64(stats.entries_restored) ||
        !valid_wire_u64(stats.entries_failed) || !valid_wire_u64(stats.bytes_restored)) {
        return invalid("partial restore stats are invalid");
    }
    if (stats.stable_error_codes.size() > kMaximumPartialRestoreErrorCodes) {
        return invalid("too many partial restore error codes");
    }
    for (const auto& code : stats.stable_error_codes) {
        if (code.empty() || code.size() > 128) {
            return invalid("partial restore error code is invalid");
        }
    }
    return base::Result<void>::success();
}

base::Result<std::vector<std::byte>>
encode_file_selection_fingerprint_preimage(const std::vector<FileSourceRef>& refs) {
    auto valid = validate_file_source_refs(refs);
    if (!valid) {
        return base::Result<std::vector<std::byte>>::failure(valid.error());
    }
    std::vector<const FileSourceRef*> ordered;
    ordered.reserve(refs.size());
    for (const auto& ref : refs) {
        ordered.push_back(&ref);
    }
    std::ranges::sort(ordered, [](const FileSourceRef* left, const FileSourceRef* right) {
        return path_sort_key(*left) < path_sort_key(*right);
    });

    // Label + algorithm + sorted selections. display_label is not authoritative and is omitted.
    constexpr std::string_view kLabel = "aegra-file-selection-fp-v1";
    std::vector<std::byte> preimage;
    preimage.reserve(64 + refs.size() * 64);
    append_string_bytes(preimage, kLabel);
    append_u8(preimage, 0);
    append_u8(preimage, kSelectionFingerprintAlgorithmSha256V1);
    append_u32_le(preimage, static_cast<std::uint32_t>(ordered.size()));
    for (const auto* ref : ordered) {
        // selection_id is canonical UUID text; encode as UTF-8 bytes with length.
        append_u32_le(preimage, static_cast<std::uint32_t>(ref->selection_id.size()));
        append_string_bytes(preimage, ref->selection_id);
        append_u32_le(preimage, static_cast<std::uint32_t>(ref->volume_identity.size()));
        append_string_bytes(preimage, ref->volume_identity);
        append_u8(preimage, static_cast<std::uint8_t>(ref->entry_kind));
        append_u8(preimage, static_cast<std::uint8_t>(ref->recursion));
        append_u8(preimage, static_cast<std::uint8_t>(ref->unreadable_policy));
        append_u32_le(preimage, static_cast<std::uint32_t>(ref->relative_components.size()));
        for (const auto& component : ref->relative_components) {
            append_u8(preimage, static_cast<std::uint8_t>(component.encoding));
            append_u32_le(preimage, static_cast<std::uint32_t>(component.bytes.size()));
            append_bytes(preimage, component.bytes);
        }
    }
    return base::Result<std::vector<std::byte>>::success(std::move(preimage));
}

} // namespace aegra::contracts
