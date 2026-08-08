#include "windows_usn_journal.h"

#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/base/error.h"

#include <Windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem::detail {
namespace {

constexpr std::size_t kUsnReadBufferBytes = 64U * 1024U;
constexpr DWORD kAllUsnReasons = 0xFFFFFFFFU;

// Content-affecting main-stream data changes.
constexpr DWORD kContentReasons = USN_REASON_DATA_OVERWRITE | USN_REASON_DATA_EXTEND |
                                  USN_REASON_DATA_TRUNCATION | USN_REASON_FILE_CREATE;

// Namespace mutations (rename/move/delete). Create is also content for planner.
constexpr DWORD kNamespaceReasons =
    USN_REASON_FILE_DELETE | USN_REASON_RENAME_OLD_NAME | USN_REASON_RENAME_NEW_NAME;

// Metadata-only when no content/namespace bits are present.
constexpr DWORD kMetadataReasons =
    USN_REASON_BASIC_INFO_CHANGE | USN_REASON_SECURITY_CHANGE | USN_REASON_EA_CHANGE |
    USN_REASON_INDEXABLE_CHANGE | USN_REASON_COMPRESSION_CHANGE | USN_REASON_ENCRYPTION_CHANGE |
    USN_REASON_OBJECT_ID_CHANGE | USN_REASON_CLOSE;

// Bits that imply unsupported objects or non-main-stream changes → ambiguous.
constexpr DWORD kAmbiguousReasons =
    USN_REASON_NAMED_DATA_OVERWRITE | USN_REASON_NAMED_DATA_EXTEND |
    USN_REASON_NAMED_DATA_TRUNCATION | USN_REASON_HARD_LINK_CHANGE |
    USN_REASON_REPARSE_POINT_CHANGE | USN_REASON_STREAM_CHANGE |
    USN_REASON_TRANSACTED_CHANGE | USN_REASON_INTEGRITY_CHANGE |
    USN_REASON_DESIRED_STORAGE_CLASS_CHANGE;

constexpr DWORD kKnownReasons =
    kContentReasons | kNamespaceReasons | kMetadataReasons | kAmbiguousReasons;

[[nodiscard]] bool is_journal_unavailable_error(const DWORD error) noexcept {
    switch (error) {
    case ERROR_INVALID_FUNCTION:
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_PARAMETER:
    case ERROR_JOURNAL_NOT_ACTIVE:
    case ERROR_JOURNAL_DELETE_IN_PROGRESS:
    case ERROR_JOURNAL_ENTRY_DELETED:
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_FILE_NOT_FOUND:
        return true;
    default:
        return false;
    }
}

[[nodiscard]] std::vector<std::uint16_t>
strip_trailing_separators(std::vector<std::uint16_t> path) {
    while (!path.empty() && (path.back() == 0 || path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    return path;
}

[[nodiscard]] contracts::FileJournalState unavailable_state(
    const std::string& volume_identity,
    const contracts::FileJournalUnavailableReason reason,
    const DWORD native_error = ERROR_SUCCESS) {
    contracts::FileJournalState state;
    state.volume_identity = volume_identity;
    state.available = false;
    state.unavailable_reason = reason;
    state.native_error_code = native_error;
    return state;
}

[[nodiscard]] contracts::FileJournalUnavailableReason
journal_unavailable_reason(const DWORD error) noexcept {
    switch (error) {
    case ERROR_JOURNAL_NOT_ACTIVE:
    case ERROR_JOURNAL_DELETE_IN_PROGRESS:
    case ERROR_JOURNAL_ENTRY_DELETED:
        return contracts::FileJournalUnavailableReason::kInactive;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return contracts::FileJournalUnavailableReason::kAccessDenied;
    default:
        return contracts::FileJournalUnavailableReason::kUnsupported;
    }
}

[[nodiscard]] base::Result<void> check_cancel(const base::CancellationToken cancellation,
                                              const char* message) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure({base::ErrorCode::kCancelled, message});
    }
    return base::Result<void>::success();
}

[[nodiscard]] std::array<std::byte, contracts::kStableFileIdBytes>
file_id_from_v2(const DWORDLONG reference) noexcept {
    std::array<std::byte, contracts::kStableFileIdBytes> file_id{};
    static_assert(sizeof(reference) == 8);
    std::memcpy(file_id.data(), &reference, sizeof(reference));
    return file_id;
}

[[nodiscard]] std::array<std::byte, contracts::kStableFileIdBytes>
file_id_from_v3(const FILE_ID_128& reference) noexcept {
    std::array<std::byte, contracts::kStableFileIdBytes> file_id{};
    static_assert(sizeof(reference.Identifier) >= contracts::kStableFileIdBytes);
    std::memcpy(file_id.data(), reference.Identifier, contracts::kStableFileIdBytes);
    return file_id;
}

struct ParsedUsnRecord final {
    std::array<std::byte, contracts::kStableFileIdBytes> file_id{};
    std::int64_t usn{0};
    std::uint32_t reason{0};
};

[[nodiscard]] base::Result<ParsedUsnRecord> parse_usn_record(const std::byte* bytes,
                                                             const std::size_t remaining) {
    if (remaining < sizeof(USN_RECORD_COMMON_HEADER)) {
        return base::Result<ParsedUsnRecord>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.usn_record_truncated"});
    }
    USN_RECORD_COMMON_HEADER header{};
    std::memcpy(&header, bytes, sizeof(header));
    if (header.RecordLength < sizeof(USN_RECORD_COMMON_HEADER) ||
        header.RecordLength > remaining || (header.RecordLength % 8U) != 0U) {
        return base::Result<ParsedUsnRecord>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.usn_record_length"});
    }
    ParsedUsnRecord parsed;
    if (header.MajorVersion == 2) {
        if (header.RecordLength < sizeof(USN_RECORD_V2)) {
            return base::Result<ParsedUsnRecord>::failure(
                {base::ErrorCode::kInvalidArgument, "file_source.usn_record_v2_short"});
        }
        USN_RECORD_V2 record{};
        std::memcpy(&record, bytes, sizeof(USN_RECORD_V2));
        parsed.file_id = file_id_from_v2(record.FileReferenceNumber);
        parsed.usn = static_cast<std::int64_t>(record.Usn);
        parsed.reason = record.Reason;
        return base::Result<ParsedUsnRecord>::success(parsed);
    }
    if (header.MajorVersion == 3) {
        if (header.RecordLength < sizeof(USN_RECORD_V3)) {
            return base::Result<ParsedUsnRecord>::failure(
                {base::ErrorCode::kInvalidArgument, "file_source.usn_record_v3_short"});
        }
        USN_RECORD_V3 record{};
        std::memcpy(&record, bytes, sizeof(USN_RECORD_V3));
        parsed.file_id = file_id_from_v3(record.FileReferenceNumber);
        parsed.usn = static_cast<std::int64_t>(record.Usn);
        parsed.reason = record.Reason;
        return base::Result<ParsedUsnRecord>::success(parsed);
    }
    // V4 and unknown major versions: refuse (eligibility / ambiguous path).
    return base::Result<ParsedUsnRecord>::failure(
        {base::ErrorCode::kInvalidArgument, "file_source.usn_record_version"});
}

[[nodiscard]] contracts::FileChangeReason merge_reason(const contracts::FileChangeReason left,
                                                       const contracts::FileChangeReason right) noexcept {
    const auto rank = [](const contracts::FileChangeReason reason) noexcept -> int {
        switch (reason) {
        case contracts::FileChangeReason::kAmbiguous:
            return 5;
        case contracts::FileChangeReason::kContent:
            return 4;
        case contracts::FileChangeReason::kNamespace:
            return 3;
        case contracts::FileChangeReason::kMetadata:
            return 2;
        case contracts::FileChangeReason::kNone:
            return 1;
        }
        return 5;
    };
    return rank(left) >= rank(right) ? left : right;
}

} // namespace

contracts::FileChangeReason map_usn_reason(const std::uint32_t reason_mask) noexcept {
    if (reason_mask == 0) {
        return contracts::FileChangeReason::kNone;
    }
    if ((reason_mask & ~kKnownReasons) != 0U) {
        return contracts::FileChangeReason::kAmbiguous;
    }
    if ((reason_mask & kAmbiguousReasons) != 0U) {
        return contracts::FileChangeReason::kAmbiguous;
    }
    if ((reason_mask & kContentReasons) != 0U) {
        return contracts::FileChangeReason::kContent;
    }
    if ((reason_mask & kNamespaceReasons) != 0U) {
        return contracts::FileChangeReason::kNamespace;
    }
    if ((reason_mask & kMetadataReasons) != 0U) {
        return contracts::FileChangeReason::kMetadata;
    }
    return contracts::FileChangeReason::kAmbiguous;
}

namespace {

constexpr DWORD kJournalShare = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;
constexpr DWORDLONG kCreatedJournalMaximumBytes = 128ULL * 1024ULL * 1024ULL;
constexpr DWORDLONG kCreatedJournalAllocationDeltaBytes = 16ULL * 1024ULL * 1024ULL;

/// Temporary DOS letter for VSS GLOBALROOT devices so CreateFile can open a true volume
/// handle (\\.\X:) that accepts FSCTL_QUERY_USN_JOURNAL. Same approach as free-skip mapping.
class TemporaryJournalDosMap final {
  public:
    TemporaryJournalDosMap() = default;
    ~TemporaryJournalDosMap() { release(); }
    TemporaryJournalDosMap(const TemporaryJournalDosMap&) = delete;
    TemporaryJournalDosMap& operator=(const TemporaryJournalDosMap&) = delete;
    TemporaryJournalDosMap(TemporaryJournalDosMap&& other) noexcept
        : dos_name_(std::move(other.dos_name_)), target_(std::move(other.target_)),
          active_(other.active_) {
        other.active_ = false;
    }
    TemporaryJournalDosMap& operator=(TemporaryJournalDosMap&& other) noexcept {
        if (this != &other) {
            release();
            dos_name_ = std::move(other.dos_name_);
            target_ = std::move(other.target_);
            active_ = other.active_;
            other.active_ = false;
        }
        return *this;
    }

    [[nodiscard]] static TemporaryJournalDosMap try_map(const std::wstring& nt_device_path) {
        TemporaryJournalDosMap map;
        if (nt_device_path.empty()) {
            return map;
        }
        for (wchar_t letter = L'Z'; letter >= L'A'; --letter) {
            const std::wstring drive_root = std::wstring(1, letter) + L":\\";
            if (GetDriveTypeW(drive_root.c_str()) != DRIVE_NO_ROOT_DIR) {
                continue;
            }
            const std::wstring dos_name = std::wstring(1, letter) + L":";
            if (DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_NO_BROADCAST_SYSTEM, dos_name.c_str(),
                                 nt_device_path.c_str()) == FALSE) {
                continue;
            }
            map.dos_name_ = dos_name;
            map.target_ = nt_device_path;
            map.active_ = true;
            return map;
        }
        return map;
    }

    [[nodiscard]] bool active() const noexcept { return active_; }

    [[nodiscard]] std::wstring win32_volume_path() const {
        if (!active_) {
            return {};
        }
        return L"\\\\.\\" + dos_name_;
    }

    void release() noexcept {
        if (!active_) {
            return;
        }
        DefineDosDeviceW(DDD_RAW_TARGET_PATH | DDD_REMOVE_DEFINITION | DDD_EXACT_MATCH_ON_REMOVE |
                             DDD_NO_BROADCAST_SYSTEM,
                         dos_name_.c_str(), target_.c_str());
        active_ = false;
        dos_name_.clear();
        target_.clear();
    }

  private:
    std::wstring dos_name_;
    std::wstring target_;
    bool active_{false};
};

[[nodiscard]] std::wstring utf16_vector_to_wstring(const std::vector<std::uint16_t>& path) {
    std::wstring value(path.begin(), path.end());
    while (!value.empty() && value.back() == L'\0') {
        value.pop_back();
    }
    return value;
}

/// Extract \Device\HarddiskVolumeShadowCopyN for DefineDosDevice(DDD_RAW_TARGET_PATH).
[[nodiscard]] std::optional<std::wstring>
nt_device_path_from_snapshot_root(const std::wstring& root) {
    constexpr std::wstring_view kGlobalRootDevice = L"\\\\?\\GLOBALROOT\\Device\\";
    if (root.size() > kGlobalRootDevice.size() &&
        _wcsnicmp(root.c_str(), kGlobalRootDevice.data(), kGlobalRootDevice.size()) == 0) {
        return std::wstring(L"\\Device\\") + root.substr(kGlobalRootDevice.size());
    }
    constexpr std::wstring_view kDevice = L"\\Device\\";
    if (root.size() > kDevice.size() &&
        _wcsnicmp(root.c_str(), kDevice.data(), kDevice.size()) == 0) {
        return root;
    }
    return std::nullopt;
}

[[nodiscard]] base::Result<UniqueHandle> try_open_journal_path(const std::wstring& path) {
    if (path.empty()) {
        return base::Result<UniqueHandle>::failure(
            {base::ErrorCode::kInvalidArgument, "snapshot volume path is empty"});
    }
    // Volume IOCTLs need a true volume open. Prefer flags=0 / GENERIC_READ first;
    // BACKUP_SEMANTICS often yields a directory-style handle that rejects USN FSCTLs
    // (ERROR_INVALID_FUNCTION) on VSS GLOBALROOT device objects.
    const DWORD access_modes[] = {GENERIC_READ, FILE_READ_ATTRIBUTES | FILE_READ_DATA,
                                  FILE_READ_ATTRIBUTES, static_cast<DWORD>(0)};
    const DWORD flag_modes[] = {static_cast<DWORD>(0), FILE_FLAG_BACKUP_SEMANTICS};
    base::Error last{base::ErrorCode::kNotFound, "CreateFileW failed for journal volume"};
    for (const auto access : access_modes) {
        for (const auto flags : flag_modes) {
            UniqueHandle handle(CreateFileW(path.c_str(), access, kJournalShare, nullptr,
                                            OPEN_EXISTING, flags, nullptr));
            if (handle.valid()) {
                return base::Result<UniqueHandle>::success(std::move(handle));
            }
            last = win32_error(GetLastError(), "CreateFileW journal volume");
        }
    }
    return base::Result<UniqueHandle>::failure(std::move(last));
}

[[nodiscard]] bool journal_query_accepted(const UniqueHandle& volume) noexcept {
    if (!volume.valid()) {
        return false;
    }
    USN_JOURNAL_DATA_V2 data{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data),
                        &bytes_returned, nullptr) == FALSE) {
        return false;
    }
    return bytes_returned >= sizeof(USN_JOURNAL_DATA_V0) && data.UsnJournalID != 0;
}

} // namespace

base::Result<bool>
ensure_file_change_journal_active(const std::vector<std::uint16_t>& live_volume_root_utf16,
                                  const base::CancellationToken cancellation) {
    auto cancelled = check_cancel(cancellation, "prepare journal cancelled");
    if (!cancelled) {
        return base::Result<bool>::failure(cancelled.error());
    }
    const auto stripped = strip_trailing_separators(live_volume_root_utf16);
    const auto volume_path = utf16_vector_to_wstring(stripped);
    if (volume_path.empty()) {
        return base::Result<bool>::failure(
            {base::ErrorCode::kInvalidArgument, "live volume path is empty"});
    }
    UniqueHandle volume(CreateFileW(volume_path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                    kJournalShare, nullptr, OPEN_EXISTING, 0, nullptr));
    if (!volume.valid()) {
        return base::Result<bool>::failure(
            win32_error(GetLastError(), "CreateFileW live journal volume"));
    }
    USN_JOURNAL_DATA_V2 current{};
    DWORD returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &current,
                        sizeof(current), &returned, nullptr) != FALSE) {
        if (returned >= sizeof(USN_JOURNAL_DATA_V0) && current.UsnJournalID != 0) {
            return base::Result<bool>::success(false);
        }
        return base::Result<bool>::failure(
            {base::ErrorCode::kIoFailure, "live journal query returned an invalid response"});
    }
    const auto query_error = GetLastError();
    if (query_error != ERROR_JOURNAL_NOT_ACTIVE) {
        return base::Result<bool>::failure(
            win32_error(query_error, "FSCTL_QUERY_USN_JOURNAL live volume"));
    }
    CREATE_USN_JOURNAL_DATA create{};
    create.MaximumSize = kCreatedJournalMaximumBytes;
    create.AllocationDelta = kCreatedJournalAllocationDeltaBytes;
    returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_CREATE_USN_JOURNAL, &create, sizeof(create), nullptr, 0,
                        &returned, nullptr) == FALSE) {
        return base::Result<bool>::failure(
            win32_error(GetLastError(), "FSCTL_CREATE_USN_JOURNAL"));
    }
    cancelled = check_cancel(cancellation, "prepare journal cancelled");
    if (!cancelled) {
        return base::Result<bool>::failure(cancelled.error());
    }
    return base::Result<bool>::success(true);
}

base::Result<UniqueHandle>
open_snapshot_volume_for_journal(const std::vector<std::uint16_t>& root_utf16) {
    auto stripped = strip_trailing_separators(root_utf16);
    if (stripped.empty()) {
        return base::Result<UniqueHandle>::failure(
            {base::ErrorCode::kInvalidArgument, "snapshot volume path is empty"});
    }
    const auto device_path = utf16_vector_to_wstring(stripped);
    const auto root_dir_path = utf16_vector_to_wstring(ensure_trailing_directory_separator(stripped));

    // 1) Device object without trailing '\\' (preferred volume form).
    if (auto handle = try_open_journal_path(device_path); handle && journal_query_accepted(handle.value())) {
        return handle;
    }
    // Keep a non-probed open as fallback if later strategies fail to open at all.
    auto fallback = try_open_journal_path(device_path);

    // 2) Root-directory form (some VSS paths only open this way).
    if (auto handle = try_open_journal_path(root_dir_path);
        handle && journal_query_accepted(handle.value())) {
        return handle;
    }
    if (!fallback) {
        fallback = try_open_journal_path(root_dir_path);
    }

    // 3) Temporary DOS letter → \\.\X: volume device (accepts USN FSCTLs reliably).
    if (const auto nt_device = nt_device_path_from_snapshot_root(device_path)) {
        auto map = TemporaryJournalDosMap::try_map(*nt_device);
        if (map.active()) {
            const auto dos_volume = map.win32_volume_path();
            if (auto handle = try_open_journal_path(dos_volume);
                handle && journal_query_accepted(handle.value())) {
                // Handle remains valid after the temporary DOS mapping is released.
                return handle;
            }
            if (!fallback) {
                fallback = try_open_journal_path(dos_volume);
            }
        }
    }

    // Last resort: any open handle (caller marks journal unavailable if query fails).
    if (fallback) {
        return fallback;
    }
    return base::Result<UniqueHandle>::failure(
        {base::ErrorCode::kIoFailure, "snapshot volume open for USN journal failed"});
}

base::Result<contracts::FileJournalState>
query_usn_journal_state(const UniqueHandle& volume, const std::string& volume_identity,
                        const base::CancellationToken cancellation) {
    auto cancelled = check_cancel(cancellation, "query journal cancelled");
    if (!cancelled) {
        return base::Result<contracts::FileJournalState>::failure(cancelled.error());
    }
    if (!volume.valid()) {
        return base::Result<contracts::FileJournalState>::failure(
            {base::ErrorCode::kInvalidArgument, "journal volume handle is invalid"});
    }
    USN_JOURNAL_DATA_V2 data{};
    DWORD bytes_returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_QUERY_USN_JOURNAL, nullptr, 0, &data, sizeof(data),
                        &bytes_returned, nullptr) == FALSE) {
        const auto error = GetLastError();
        if (is_journal_unavailable_error(error)) {
            // Snapshot journal unsupported or inactive → Full-downgrade path, not hard fail.
            return base::Result<contracts::FileJournalState>::success(
                unavailable_state(volume_identity, journal_unavailable_reason(error), error));
        }
        return base::Result<contracts::FileJournalState>::failure(
            win32_error(error, "FSCTL_QUERY_USN_JOURNAL"));
    }
    if (bytes_returned < sizeof(USN_JOURNAL_DATA_V0) || data.UsnJournalID == 0) {
        return base::Result<contracts::FileJournalState>::success(
            unavailable_state(volume_identity,
                              contracts::FileJournalUnavailableReason::kInvalidResponse));
    }
    contracts::FileJournalState state;
    state.volume_identity = volume_identity;
    state.journal_id = static_cast<std::uint64_t>(data.UsnJournalID);
    state.lowest_valid_usn = static_cast<std::int64_t>(data.LowestValidUsn);
    state.next_usn = static_cast<std::int64_t>(data.NextUsn);
    state.available = state.lowest_valid_usn >= 0 && state.next_usn >= state.lowest_valid_usn;
    if (!state.available) {
        return base::Result<contracts::FileJournalState>::success(
            unavailable_state(volume_identity,
                              contracts::FileJournalUnavailableReason::kInvalidResponse));
    }
    return base::Result<contracts::FileJournalState>::success(std::move(state));
}

base::Result<contracts::FileChangeBatch>
read_usn_change_batch(const UniqueHandle& volume, const std::string& volume_identity,
                      const std::uint64_t journal_id, const std::int64_t start_usn,
                      const std::int64_t end_usn, const std::uint32_t maximum_hints,
                      const base::CancellationToken cancellation) {
    auto cancelled = check_cancel(cancellation, "read change batch cancelled");
    if (!cancelled) {
        return base::Result<contracts::FileChangeBatch>::failure(cancelled.error());
    }
    if (!volume.valid() || journal_id == 0 || maximum_hints == 0 ||
        maximum_hints > contracts::kMaximumChangeHintsPerBatch || start_usn < 0 || end_usn < 0 ||
        start_usn > end_usn) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kInvalidArgument, "change batch range is invalid"});
    }
    contracts::FileChangeBatch batch;
    if (start_usn == end_usn) {
        return base::Result<contracts::FileChangeBatch>::success(std::move(batch));
    }

    READ_USN_JOURNAL_DATA_V1 read_input{};
    read_input.StartUsn = static_cast<USN>(start_usn);
    read_input.ReasonMask = kAllUsnReasons;
    read_input.ReturnOnlyOnClose = FALSE;
    read_input.Timeout = 0;
    read_input.BytesToWaitFor = 0;
    read_input.UsnJournalID = static_cast<DWORDLONG>(journal_id);
    read_input.MinMajorVersion = 2;
    read_input.MaxMajorVersion = 3;

    std::vector<std::byte> buffer(kUsnReadBufferBytes);
    DWORD bytes_returned = 0;
    if (DeviceIoControl(volume.get(), FSCTL_READ_USN_JOURNAL, &read_input, sizeof(read_input),
                        buffer.data(), static_cast<DWORD>(buffer.size()), &bytes_returned,
                        nullptr) == FALSE) {
        const auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF) {
            // No more records in the requested window.
            return base::Result<contracts::FileChangeBatch>::success(std::move(batch));
        }
        if (is_journal_unavailable_error(error)) {
            return base::Result<contracts::FileChangeBatch>::failure(
                {base::ErrorCode::kConflict, "file_backup.journal_inaccessible"});
        }
        return base::Result<contracts::FileChangeBatch>::failure(
            win32_error(error, "FSCTL_READ_USN_JOURNAL"));
    }
    if (bytes_returned < sizeof(USN)) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kIoFailure, "file_source.usn_read_truncated"});
    }

    USN next_usn_cursor = 0;
    std::memcpy(&next_usn_cursor, buffer.data(), sizeof(next_usn_cursor));
    std::size_t offset = sizeof(USN);
    std::int64_t last_emitted_usn = start_usn - 1;

    while (offset + sizeof(USN_RECORD_COMMON_HEADER) <= bytes_returned &&
           batch.hints.size() < maximum_hints) {
        if (cancellation.stop_requested()) {
            return base::Result<contracts::FileChangeBatch>::failure(
                {base::ErrorCode::kCancelled, "read change batch cancelled"});
        }
        auto parsed = parse_usn_record(buffer.data() + offset, bytes_returned - offset);
        if (!parsed) {
            return base::Result<contracts::FileChangeBatch>::failure(parsed.error());
        }
        USN_RECORD_COMMON_HEADER header{};
        std::memcpy(&header, buffer.data() + offset, sizeof(header));
        offset += header.RecordLength;

        if (parsed.value().usn < start_usn) {
            continue;
        }
        if (parsed.value().usn >= end_usn) {
            // Reached exclusive end; range exhausted.
            return base::Result<contracts::FileChangeBatch>::success(std::move(batch));
        }
        last_emitted_usn = parsed.value().usn;
        const auto reason = map_usn_reason(parsed.value().reason);
        if (reason == contracts::FileChangeReason::kNone) {
            continue;
        }
        contracts::FileChangeHint hint;
        hint.identity.volume_identity = volume_identity;
        hint.identity.file_id = parsed.value().file_id;
        hint.reason = reason;
        // Coalesce consecutive same-identity records within this buffer slice.
        if (!batch.hints.empty() && batch.hints.back().identity == hint.identity) {
            batch.hints.back().reason = merge_reason(batch.hints.back().reason, reason);
        } else {
            batch.hints.push_back(std::move(hint));
        }
    }

    const auto next_cursor = static_cast<std::int64_t>(next_usn_cursor);
    if (batch.hints.size() >= maximum_hints) {
        // Caller continues from after the last emitted USN.
        batch.next_start_usn = last_emitted_usn + 1;
    } else if (next_cursor > start_usn && next_cursor < end_usn) {
        batch.next_start_usn = next_cursor;
    } else if (next_cursor >= end_usn || next_cursor <= start_usn) {
        // Exhausted for this query window.
        batch.next_start_usn = std::nullopt;
    }
    return base::Result<contracts::FileChangeBatch>::success(std::move(batch));
}

} // namespace aegra::adapters::windows_filesystem::detail

namespace aegra::adapters::windows_filesystem {

base::Result<bool>
ensure_file_change_journal_active(const std::vector<std::uint16_t>& live_volume_root_utf16,
                                  const base::CancellationToken cancellation) {
    return detail::ensure_file_change_journal_active(live_volume_root_utf16, cancellation);
}

} // namespace aegra::adapters::windows_filesystem
