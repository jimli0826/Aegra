#include "aegra/adapters/windows_filesystem/windows_filesystem.h"

#include "windows_file_handle.h"
#include "windows_file_names.h"
#include "windows_file_security.h"
#include "windows_usn_journal.h"

#include "aegra/base/error.h"
#include "aegra/format/file_index.h"

#include <Windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <deque>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem {
namespace {

using detail::UniqueHandle;
using detail::win32_error;

[[nodiscard]] base::Error unreadable_source_error(const DWORD error, std::string operation) {
    auto result = win32_error(error, std::move(operation));
    result.message.insert(0, "file_source.unreadable: ");
    return result;
}

struct RegisteredEntry final {
    contracts::FileEntryDesc desc;
    std::wstring absolute_path;
};

struct VolumeRoot final {
    std::string volume_identity;
    std::vector<std::uint16_t> root_utf16;
    /// Snapshot volume handle for USN FSCTLs (opened lazily; never live volume).
    detail::UniqueHandle journal_handle;
    std::uint64_t journal_id{0};
    bool journal_probed{false};
    bool journal_available{false};
    contracts::FileJournalUnavailableReason journal_unavailable_reason{
        contracts::FileJournalUnavailableReason::kNone};
    std::uint32_t journal_native_error_code{0};
};

[[nodiscard]] contracts::FileEntryKind classify(const DWORD attributes) noexcept {
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
        return contracts::FileEntryKind::kDirectory;
    }
    return contracts::FileEntryKind::kFile;
}

[[nodiscard]] bool is_named_data_stream(const std::wstring_view name) noexcept {
    constexpr std::wstring_view kUnnamedDataStream = L"::$DATA";
    constexpr std::wstring_view kDataSuffix = L":$DATA";
    return name != kUnnamedDataStream && name.ends_with(kDataSuffix);
}

[[nodiscard]] base::Result<bool> has_named_data_stream(const HANDLE handle) {
    constexpr std::size_t kInitialBufferBytes = 64U * 1024U;
    constexpr std::size_t kMaximumBufferBytes = 1024U * 1024U;
    std::vector<std::byte> buffer(kInitialBufferBytes);
    for (;;) {
        if (GetFileInformationByHandleEx(handle, FileStreamInfo, buffer.data(),
                                         static_cast<DWORD>(buffer.size())) != FALSE) {
            break;
        }
        const auto error = GetLastError();
        if (error == ERROR_HANDLE_EOF) {
            return base::Result<bool>::success(false);
        }
        if ((error != ERROR_MORE_DATA && error != ERROR_INSUFFICIENT_BUFFER) ||
            buffer.size() >= kMaximumBufferBytes) {
            return base::Result<bool>::failure(unreadable_source_error(error, "FileStreamInfo"));
        }
        buffer.resize((std::min)(buffer.size() * 2U, kMaximumBufferBytes));
    }
    std::size_t offset = 0;
    for (;;) {
        if (offset > buffer.size() - offsetof(FILE_STREAM_INFO, StreamName)) {
            return base::Result<bool>::failure(
                {base::ErrorCode::kIoFailure, "file_source.unreadable: invalid stream offset"});
        }
        // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast)
        const auto* info = reinterpret_cast<const FILE_STREAM_INFO*>(buffer.data() + offset);
        if (info->StreamNameLength % sizeof(wchar_t) != 0 ||
            info->StreamNameLength > buffer.size() - offset - offsetof(FILE_STREAM_INFO, StreamName)) {
            return base::Result<bool>::failure(
                {base::ErrorCode::kIoFailure, "file_source.unreadable: invalid stream name"});
        }
        const std::wstring_view name(info->StreamName, info->StreamNameLength / sizeof(wchar_t));
        if (is_named_data_stream(name)) {
            return base::Result<bool>::success(true);
        }
        if (info->NextEntryOffset == 0) {
            return base::Result<bool>::success(false);
        }
        if (info->NextEntryOffset < offsetof(FILE_STREAM_INFO, StreamName) ||
            info->NextEntryOffset > buffer.size() - offset) {
            return base::Result<bool>::failure(
                {base::ErrorCode::kIoFailure, "file_source.unreadable: invalid next stream offset"});
        }
        offset += info->NextEntryOffset;
    }
}

/// FI0: strict fail on reparse / hard-linked file / sparse / named ADS.
/// Also captures FILE_ID_128 for FI1 stable identity (no USN).
/// Paths must not appear in the stable message code.
[[nodiscard]] base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>
reject_unsupported_and_read_file_id(const std::wstring& absolute_path, const DWORD attributes) {
    if ((attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.unsupported_reparse"});
    }
    if ((attributes & FILE_ATTRIBUTE_SPARSE_FILE) != 0) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.unsupported_sparse"});
    }
    const bool is_directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    auto handle = detail::open_path(
        detail::to_utf16_vector(absolute_path), FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
    if (!handle) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            {handle.error().code, "file_source.unreadable"});
    }
    if (!is_directory) {
        BY_HANDLE_FILE_INFORMATION info{};
        if (GetFileInformationByHandle(handle.value().get(), &info) == FALSE) {
            return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
                unreadable_source_error(GetLastError(), "GetFileInformationByHandle"));
        }
        if (info.nNumberOfLinks > 1) {
            return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
                {base::ErrorCode::kInvalidArgument, "file_source.unsupported_hard_link"});
        }
    }
    FILE_ID_INFO id_info{};
    if (GetFileInformationByHandleEx(handle.value().get(), FileIdInfo, &id_info,
                                     sizeof(id_info)) == FALSE) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            unreadable_source_error(GetLastError(), "FileIdInfo"));
    }
    std::array<std::byte, contracts::kStableFileIdBytes> file_id{};
    static_assert(sizeof(id_info.FileId.Identifier) >= contracts::kStableFileIdBytes);
    std::memcpy(file_id.data(), id_info.FileId.Identifier, contracts::kStableFileIdBytes);

    auto named_data_stream = has_named_data_stream(handle.value().get());
    if (!named_data_stream) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            named_data_stream.error());
    }
    if (named_data_stream.value()) {
        return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.unsupported_ads"});
    }
    return base::Result<std::array<std::byte, contracts::kStableFileIdBytes>>::success(file_id);
}

class SnapshotEnumerator final : public ports::IFileTreeEnumerator {
  public:
    SnapshotEnumerator(std::wstring root_path, const contracts::FileSourceRef selection,
                       std::unordered_map<std::uint64_t, RegisteredEntry>& registry,
                       std::uint64_t& next_entry_id)
        : root_path_(std::move(root_path)), selection_(selection), registry_(registry),
          next_entry_id_(next_entry_id) {
        Pending root;
        root.absolute_path = root_path_;
        root.parent_entry_id = 0;
        root.depth = 0;
        // Append relative components under snapshot root.
        for (const auto& component : selection_.relative_components) {
            std::wstring piece(component.bytes.size() / 2U, L'\0');
            if (!component.bytes.empty()) {
                std::memcpy(piece.data(), component.bytes.data(), component.bytes.size());
            }
            if (root.absolute_path.empty() ||
                (root.absolute_path.back() != L'\\' && root.absolute_path.back() != L'/')) {
                root.absolute_path.push_back(L'\\');
            }
            root.absolute_path.append(piece);
            ++root.depth;
        }
        pending_.push_back(std::move(root));
        seed_selection_root_ = true;
    }

    base::Result<ports::FileEnumerateBatch>
    next_batch(const std::uint32_t maximum_entries,
               const base::CancellationToken cancellation) override {
        ports::FileEnumerateBatch batch;
        if (maximum_entries == 0) {
            return base::Result<ports::FileEnumerateBatch>::failure(
                {base::ErrorCode::kInvalidArgument, "enumerate batch size must be positive"});
        }
        while (batch.entries.size() < maximum_entries) {
            if (cancellation.stop_requested()) {
                return base::Result<ports::FileEnumerateBatch>::failure(
                    {base::ErrorCode::kCancelled, "file enumeration cancelled"});
            }
            if (seed_selection_root_) {
                seed_selection_root_ = false;
                auto seeded = emit_path(pending_.front().absolute_path, 0, 0, cancellation);
                if (!seeded) {
                    return base::Result<ports::FileEnumerateBatch>::failure(seeded.error());
                }
                if (seeded.value().kind == contracts::FileEntryKind::kDirectory &&
                    selection_.recursion == contracts::FileRecursion::kRecursive) {
                    pending_.front().parent_entry_id = seeded.value().entry_id;
                    auto queued = queue_children(pending_.front());
                    if (!queued) {
                        return base::Result<ports::FileEnumerateBatch>::failure(queued.error());
                    }
                }
                pending_.pop_front();
                batch.entries.push_back(std::move(seeded).value());
                continue;
            }
            if (pending_.empty()) {
                break;
            }
            auto current = pending_.front();
            pending_.pop_front();
            auto emitted =
                emit_path(current.absolute_path, current.parent_entry_id, current.depth, cancellation);
            if (!emitted) {
                return base::Result<ports::FileEnumerateBatch>::failure(emitted.error());
            }
            if (emitted.value().kind == contracts::FileEntryKind::kDirectory &&
                selection_.recursion == contracts::FileRecursion::kRecursive &&
                current.depth < contracts::kMaximumFileDirectoryDepth) {
                Pending child_dir = current;
                child_dir.parent_entry_id = emitted.value().entry_id;
                auto queued = queue_children(child_dir);
                if (!queued) {
                    return base::Result<ports::FileEnumerateBatch>::failure(queued.error());
                }
            }
            batch.entries.push_back(std::move(emitted).value());
        }
        if (!pending_.empty() || seed_selection_root_) {
            batch.continuation_token = "1";
        }
        return base::Result<ports::FileEnumerateBatch>::success(std::move(batch));
    }

  private:
    struct Pending final {
        std::wstring absolute_path;
        std::uint64_t parent_entry_id{0};
        std::uint32_t depth{0};
    };

    [[nodiscard]] static std::wstring join_under(const std::wstring& directory,
                                                 const std::wstring_view name) {
        if (directory.empty()) {
            return std::wstring(name);
        }
        if (directory.back() == L'\\' || directory.back() == L'/') {
            return directory + std::wstring(name);
        }
        return directory + L'\\' + std::wstring(name);
    }

    /// fail_job (V1 fixed): access denied / I/O / snapshot anomalies must not look like empty dirs.
    [[nodiscard]] static base::Error directory_enumerate_error(const DWORD error,
                                                               std::string operation) {
        auto err = win32_error(error, std::move(operation));
        // Stable product code (B02); pipeline/session Abort — no incomplete visible RP.
        err.message = "file_source.unreadable";
        return err;
    }

    [[nodiscard]] base::Result<void> queue_children(const Pending& directory) {
        const auto pattern = join_under(directory.absolute_path, L"*");
        WIN32_FIND_DATAW data{};
        const HANDLE find = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data,
                                             FindExSearchNameMatch, nullptr, 0);
        if (find == INVALID_HANDLE_VALUE) {
            // Empty directories still return "." / ".." — FindFirst failure is never "empty".
            return base::Result<void>::failure(
                directory_enumerate_error(GetLastError(), "FindFirstFileExW"));
        }
        do {
            const std::wstring_view name(data.cFileName);
            if (name == L"." || name == L"..") {
                continue;
            }
            Pending child;
            child.absolute_path = join_under(directory.absolute_path, name);
            child.parent_entry_id = directory.parent_entry_id;
            child.depth = directory.depth + 1;
            pending_.push_back(std::move(child));
        } while (FindNextFileW(find, &data) != FALSE);
        const DWORD next_error = GetLastError();
        FindClose(find);
        // FindNextFileW sets ERROR_NO_MORE_FILES on normal end; any other code is hard fail.
        if (next_error != ERROR_NO_MORE_FILES) {
            return base::Result<void>::failure(
                directory_enumerate_error(next_error, "FindNextFileW"));
        }
        return base::Result<void>::success();
    }

    [[nodiscard]] base::Result<contracts::FileEntryDesc>
    emit_path(const std::wstring& absolute_path, const std::uint64_t parent_entry_id,
              const std::uint32_t depth, const base::CancellationToken cancellation) {
        if (cancellation.stop_requested()) {
            return base::Result<contracts::FileEntryDesc>::failure(
                {base::ErrorCode::kCancelled, "file enumeration cancelled"});
        }
        if (depth > contracts::kMaximumFileDirectoryDepth) {
            return base::Result<contracts::FileEntryDesc>::failure(
                {base::ErrorCode::kInvalidArgument, "file_source.directory_depth_exceeded"});
        }
        const DWORD attributes = GetFileAttributesW(absolute_path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return base::Result<contracts::FileEntryDesc>::failure(
                win32_error(GetLastError(), "GetFileAttributesW"));
        }
        if ((attributes & FILE_ATTRIBUTE_ENCRYPTED) != 0) {
            return base::Result<contracts::FileEntryDesc>::failure(
                {base::ErrorCode::kInvalidArgument, "file_source.unsupported_efs"});
        }
        auto unsupported = reject_unsupported_and_read_file_id(absolute_path, attributes);
        if (!unsupported) {
            return base::Result<contracts::FileEntryDesc>::failure(unsupported.error());
        }
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(absolute_path.c_str(), GetFileExInfoStandard, &info) == FALSE) {
            return base::Result<contracts::FileEntryDesc>::failure(
                win32_error(GetLastError(), "GetFileAttributesExW"));
        }
        contracts::FileEntryDesc entry;
        entry.entry_id = next_entry_id_++;
        entry.parent_entry_id = parent_entry_id;
        entry.kind = classify(attributes);
        entry.stable_identity.volume_identity = selection_.volume_identity;
        entry.stable_identity.file_id = unsupported.value();
        // Strip bits that must never appear on supported objects (defense in depth).
        entry.attributes =
            attributes & ~(FILE_ATTRIBUTE_REPARSE_POINT | FILE_ATTRIBUTE_SPARSE_FILE);
        entry.creation_time = (static_cast<std::uint64_t>(info.ftCreationTime.dwHighDateTime) << 32U) |
                              info.ftCreationTime.dwLowDateTime;
        entry.access_time = (static_cast<std::uint64_t>(info.ftLastAccessTime.dwHighDateTime) << 32U) |
                            info.ftLastAccessTime.dwLowDateTime;
        entry.write_time = (static_cast<std::uint64_t>(info.ftLastWriteTime.dwHighDateTime) << 32U) |
                           info.ftLastWriteTime.dwLowDateTime;
        entry.change_time = entry.write_time;
        const auto slash = absolute_path.find_last_of(L'\\');
        const auto leaf = slash == std::wstring::npos ? absolute_path : absolute_path.substr(slash + 1);
        if (parent_entry_id == 0 && selection_.relative_components.empty()) {
            const std::wstring selection_root_name(selection_.selection_id.begin(),
                                                   selection_.selection_id.end());
            entry.name = detail::make_utf16_name(selection_root_name);
        } else {
            entry.name = detail::make_utf16_name(leaf);
        }
        if (entry.kind == contracts::FileEntryKind::kFile) {
            entry.logical_size =
                (static_cast<std::uint64_t>(info.nFileSizeHigh) << 32U) | info.nFileSizeLow;
            contracts::FileStreamDesc main_stream;
            main_stream.stream_index = 0; // assigned by pipeline
            main_stream.stream_kind = contracts::FileStreamKind::kMain;
            main_stream.logical_size = entry.logical_size;
            entry.streams.push_back(std::move(main_stream));
        }
        // ADR-0016 / V7 §5.7: Owner/Group/DACL/SACL self-relative SD; SACL unreadable is hard fail.
        auto security = detail::read_self_relative_security_descriptor(absolute_path);
        if (!security) {
            return base::Result<contracts::FileEntryDesc>::failure(security.error());
        }
        auto envelope =
            format::file_index::encode_platform_security_envelope(security.value());
        if (!envelope) {
            return base::Result<contracts::FileEntryDesc>::failure(envelope.error());
        }
        entry.platform_metadata = std::move(envelope).value();
        entry.flags |= contracts::kEntryFlagHasSecurity;
        RegisteredEntry registered;
        registered.desc = entry;
        registered.absolute_path = absolute_path;
        registry_.emplace(entry.entry_id, std::move(registered));
        return base::Result<contracts::FileEntryDesc>::success(std::move(entry));
    }

    std::wstring root_path_;
    contracts::FileSourceRef selection_;
    std::unordered_map<std::uint64_t, RegisteredEntry>& registry_;
    std::uint64_t& next_entry_id_;
    std::deque<Pending> pending_;
    bool seed_selection_root_{false};
};

class SnapshotContentReader final : public ports::IFileContentReader {
  public:
    SnapshotContentReader(UniqueHandle handle, const std::uint64_t size)
        : handle_(std::move(handle)), size_(size) {}

    std::uint64_t size_bytes() const noexcept override { return size_; }

    base::Result<std::size_t> read(const std::uint64_t offset, const std::span<std::byte> destination,
                                   const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                {base::ErrorCode::kCancelled, "file read cancelled"});
        }
        if (destination.empty()) {
            return base::Result<std::size_t>::success(0);
        }
        if (offset > size_) {
            return base::Result<std::size_t>::failure(
                {base::ErrorCode::kInvalidArgument, "file read offset is past end"});
        }
        if (offset == size_) {
            return base::Result<std::size_t>::success(0);
        }
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle_.get(), distance, nullptr, FILE_BEGIN) == FALSE) {
            return base::Result<std::size_t>::failure(win32_error(GetLastError(), "SetFilePointerEx"));
        }
        const auto to_read = static_cast<DWORD>((std::min)(
            destination.size(), static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
        DWORD got = 0;
        if (ReadFile(handle_.get(), destination.data(), to_read, &got, nullptr) == FALSE) {
            return base::Result<std::size_t>::failure(win32_error(GetLastError(), "ReadFile"));
        }
        return base::Result<std::size_t>::success(got);
    }

  private:
    UniqueHandle handle_;
    std::uint64_t size_{0};
};

} // namespace

struct WindowsFileSnapshotView::Impl final {
    std::vector<VolumeRoot> volumes;
    std::unordered_map<std::uint64_t, RegisteredEntry> registry;
    std::uint64_t next_entry_id{1};
};

WindowsFileSnapshotView::WindowsFileSnapshotView(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WindowsFileSnapshotView::~WindowsFileSnapshotView() = default;

base::Result<std::unique_ptr<WindowsFileSnapshotView>>
WindowsFileSnapshotView::open(const WindowsFileSnapshotOpenRequest& request) {
    if (request.volumes.empty()) {
        return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::failure(
            {base::ErrorCode::kInvalidArgument, "snapshot volume bindings are required"});
    }
    auto privileges = detail::enable_file_backup_privileges();
    if (!privileges) {
        return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::failure(privileges.error());
    }
    auto implementation = std::make_unique<Impl>();
    for (const auto& volume : request.volumes) {
        if (volume.volume_identity.empty() || volume.snapshot_root_utf16.empty()) {
            return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::failure(
                {base::ErrorCode::kInvalidArgument, "snapshot volume binding is incomplete"});
        }
        // VSS device objects (\\?\GLOBALROOT\Device\HarddiskVolumeShadowCopyN) open as raw
        // volume handles without a trailing '\\'. Directory open + ByHandle volume info need
        // the root-directory form.
        auto root_utf16 = detail::ensure_trailing_directory_separator(volume.snapshot_root_utf16);
        auto handle = detail::open_path(root_utf16, FILE_READ_ATTRIBUTES,
                                        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                        OPEN_EXISTING,
                                        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT);
        if (!handle) {
            return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::failure(handle.error());
        }
        auto fs = detail::ensure_ntfs_or_refs(handle.value());
        if (!fs) {
            // Some shadow-copy roots still reject ByHandle (Win32 error 1); fall back to path API.
            std::wstring root_path(root_utf16.begin(), root_utf16.end());
            fs = detail::ensure_ntfs_or_refs_path(root_path);
            if (!fs) {
                return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::failure(fs.error());
            }
        }
        VolumeRoot root;
        root.volume_identity = volume.volume_identity;
        root.root_utf16 = std::move(root_utf16);
        implementation->volumes.push_back(std::move(root));
    }
    return base::Result<std::unique_ptr<WindowsFileSnapshotView>>::success(
        std::unique_ptr<WindowsFileSnapshotView>(
            new WindowsFileSnapshotView(std::move(implementation))));
}

base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>
WindowsFileSnapshotView::open_enumerator(const contracts::FileSourceRef& selection,
                                         const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>::failure(
            {base::ErrorCode::kCancelled, "open enumerator cancelled"});
    }
    auto validated = contracts::validate_file_source_ref(selection);
    if (!validated) {
        return base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>::failure(validated.error());
    }
    const auto volume = std::find_if(
        implementation_->volumes.begin(), implementation_->volumes.end(),
        [&](const VolumeRoot& candidate) {
            return candidate.volume_identity == selection.volume_identity;
        });
    if (volume == implementation_->volumes.end()) {
        return base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>::failure(
            {base::ErrorCode::kNotFound, "selection volume is not in snapshot set"});
    }
    std::wstring root(volume->root_utf16.begin(), volume->root_utf16.end());
    return base::Result<std::unique_ptr<ports::IFileTreeEnumerator>>::success(
        std::make_unique<SnapshotEnumerator>(std::move(root), selection, implementation_->registry,
                                             implementation_->next_entry_id));
}

base::Result<std::unique_ptr<ports::IFileContentReader>>
WindowsFileSnapshotView::open_stream_reader(const std::uint64_t entry_id,
                                            const std::uint32_t stream_index,
                                            const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IFileContentReader>>::failure(
            {base::ErrorCode::kCancelled, "open stream cancelled"});
    }
    const auto found = implementation_->registry.find(entry_id);
    if (found == implementation_->registry.end()) {
        return base::Result<std::unique_ptr<ports::IFileContentReader>>::failure(
            {base::ErrorCode::kNotFound, "snapshot entry was not found"});
    }
    // F4 first cut: only main data stream (index assigned by pipeline, matched by kind).
    const auto& entry = found->second.desc;
    const contracts::FileStreamDesc* stream = nullptr;
    for (const auto& candidate : entry.streams) {
        if (candidate.stream_index == stream_index ||
            (stream_index != 0 && candidate.stream_kind == contracts::FileStreamKind::kMain &&
             candidate.stream_index == 0)) {
            stream = &candidate;
            break;
        }
    }
    if (stream == nullptr && entry.streams.size() == 1 &&
        entry.streams.front().stream_kind == contracts::FileStreamKind::kMain) {
        stream = &entry.streams.front();
    }
    if (stream == nullptr) {
        return base::Result<std::unique_ptr<ports::IFileContentReader>>::failure(
            {base::ErrorCode::kNotFound, "snapshot stream was not found"});
    }
    std::vector<std::uint16_t> path = detail::to_utf16_vector(found->second.absolute_path);
    auto handle = detail::open_path(path, GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_SEQUENTIAL_SCAN);
    if (!handle) {
        return base::Result<std::unique_ptr<ports::IFileContentReader>>::failure(handle.error());
    }
    return base::Result<std::unique_ptr<ports::IFileContentReader>>::success(
        std::make_unique<SnapshotContentReader>(std::move(handle).value(), stream->logical_size));
}

namespace {

[[nodiscard]] base::Result<void>
ensure_journal_handle(VolumeRoot& volume, const base::CancellationToken cancellation) {
    if (volume.journal_probed) {
        return base::Result<void>::success();
    }
    auto handle = detail::open_snapshot_volume_for_journal(volume.root_utf16);
    if (!handle) {
        // Cannot open snapshot volume for journal → unavailable (Full downgrade), not hard fail.
        volume.journal_probed = true;
        volume.journal_available = false;
        volume.journal_unavailable_reason =
            contracts::FileJournalUnavailableReason::kOpenFailed;
        return base::Result<void>::success();
    }
    auto state =
        detail::query_usn_journal_state(handle.value(), volume.volume_identity, cancellation);
    if (!state) {
        // Leave unprobed on cancel so a later call can retry; other errors surface to caller.
        return base::Result<void>::failure(state.error());
    }
    volume.journal_handle = std::move(handle).value();
    volume.journal_available = state.value().available;
    volume.journal_id = state.value().journal_id;
    volume.journal_unavailable_reason = state.value().unavailable_reason;
    volume.journal_native_error_code = state.value().native_error_code;
    volume.journal_probed = true;
    return base::Result<void>::success();
}

} // namespace

base::Result<contracts::FileJournalState>
WindowsFileSnapshotView::query_journal_state(const std::string& volume_identity,
                                             const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileJournalState>::failure(
            {base::ErrorCode::kCancelled, "query journal cancelled"});
    }
    const auto volume = std::find_if(
        implementation_->volumes.begin(), implementation_->volumes.end(),
        [&](const VolumeRoot& candidate) { return candidate.volume_identity == volume_identity; });
    if (volume == implementation_->volumes.end()) {
        return base::Result<contracts::FileJournalState>::failure(
            {base::ErrorCode::kNotFound, "journal volume is not in snapshot set"});
    }
    auto ensured = ensure_journal_handle(*volume, cancellation);
    if (!ensured) {
        return base::Result<contracts::FileJournalState>::failure(ensured.error());
    }
    if (!volume->journal_available || !volume->journal_handle.valid()) {
        contracts::FileJournalState state;
        state.volume_identity = volume_identity;
        state.available = false;
        state.unavailable_reason = volume->journal_unavailable_reason;
        state.native_error_code = volume->journal_native_error_code;
        return base::Result<contracts::FileJournalState>::success(std::move(state));
    }
    // Re-query on the same snapshot handle so NextUsn reflects the frozen view.
    return detail::query_usn_journal_state(volume->journal_handle, volume_identity, cancellation);
}

base::Result<contracts::FileChangeBatch>
WindowsFileSnapshotView::read_change_batch(const std::string& volume_identity,
                                           const std::int64_t start_usn, const std::int64_t end_usn,
                                           const std::uint32_t maximum_hints,
                                           const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kCancelled, "read change batch cancelled"});
    }
    if (maximum_hints == 0 || maximum_hints > contracts::kMaximumChangeHintsPerBatch ||
        start_usn < 0 || end_usn < 0 || start_usn > end_usn) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kInvalidArgument, "change batch range is invalid"});
    }
    const auto volume = std::find_if(
        implementation_->volumes.begin(), implementation_->volumes.end(),
        [&](const VolumeRoot& candidate) { return candidate.volume_identity == volume_identity; });
    if (volume == implementation_->volumes.end()) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kNotFound, "journal volume is not in snapshot set"});
    }
    auto ensured = ensure_journal_handle(*volume, cancellation);
    if (!ensured) {
        return base::Result<contracts::FileChangeBatch>::failure(ensured.error());
    }
    if (!volume->journal_available || !volume->journal_handle.valid() || volume->journal_id == 0) {
        return base::Result<contracts::FileChangeBatch>::failure(
            {base::ErrorCode::kConflict, "file_backup.journal_inaccessible"});
    }
    return detail::read_usn_change_batch(volume->journal_handle, volume_identity,
                                         volume->journal_id, start_usn, end_usn, maximum_hints,
                                         cancellation);
}

} // namespace aegra::adapters::windows_filesystem
