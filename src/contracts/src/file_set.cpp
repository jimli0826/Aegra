#include "aegra/contracts/file_set.h"

#include "aegra/base/uuid.h"

#include <algorithm>
#include <set>
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
    return !value.empty() && value.size() <= 512;
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

} // namespace

bool is_known_content_kind(const ContentKind kind) noexcept {
    return kind == ContentKind::kVolumeSet || kind == ContentKind::kFileSet;
}

bool is_known_name_encoding(const NameEncoding encoding) noexcept {
    return encoding == NameEncoding::kWindowsUtf16Le;
}

bool is_known_file_entry_kind(const FileEntryKind kind) noexcept {
    return kind == FileEntryKind::kDirectory || kind == FileEntryKind::kFile ||
           kind == FileEntryKind::kReparse || kind == FileEntryKind::kOther;
}

bool is_known_file_recursion(const FileRecursion recursion) noexcept {
    return recursion == FileRecursion::kSelfOnly || recursion == FileRecursion::kRecursive;
}

bool is_known_file_conflict_policy(const FileConflictPolicy policy) noexcept {
    return policy == FileConflictPolicy::kFail || policy == FileConflictPolicy::kReplace ||
           policy == FileConflictPolicy::kRename;
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
        ref.reparse_policy != FileReparsePolicy::kCaptureNoFollow ||
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

base::Result<void> validate_file_restore_target(const FileRestoreTarget& target) {
    if (target.target_root_identity.empty() ||
        target.target_root_identity.size() > kMaximumTargetRootIdentityBytes) {
        return invalid("file restore target_root_identity is invalid");
    }
    if (target.entry_ids.empty() || target.entry_ids.size() > kMaximumFileRestoreEntryIds) {
        return invalid("file restore entry_ids count is invalid");
    }
    if (!is_known_file_conflict_policy(target.conflict_policy) || !target.restore_security ||
        !target.restore_ads) {
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

} // namespace aegra::contracts
