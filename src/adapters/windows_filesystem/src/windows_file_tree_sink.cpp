#include "aegra/adapters/windows_filesystem/windows_filesystem.h"

#include "windows_file_handle.h"
#include "windows_file_names.h"
#include "windows_file_security.h"

#include "aegra/base/error.h"
#include "aegra/format/file_index.h"

#include <Windows.h>
#include <winioctl.h>

#include <cstdint>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_filesystem {
namespace {

using detail::UniqueHandle;
using detail::win32_error;

[[nodiscard]] bool path_exists(const std::wstring& path) noexcept {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

[[nodiscard]] bool is_directory_attrs(const DWORD attributes) noexcept {
    return (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

void clear_readonly_attribute(const std::wstring& path) noexcept {
    const auto attributes = GetFileAttributesW(path.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        return;
    }
    if ((attributes & FILE_ATTRIBUTE_READONLY) != 0) {
        SetFileAttributesW(path.c_str(), attributes & ~static_cast<DWORD>(FILE_ATTRIBUTE_READONLY));
    }
}

/// Builds "name (N).ext" candidates until a free path is found (N = 1..9999).
[[nodiscard]] base::Result<std::wstring> allocate_rename_path(const std::wstring& final_path) {
    std::wstring directory;
    std::wstring filename = final_path;
    const auto slash = final_path.find_last_of(L"\\/");
    if (slash != std::wstring::npos) {
        directory = final_path.substr(0, slash + 1);
        filename = final_path.substr(slash + 1);
    }
    std::wstring stem = filename;
    std::wstring extension;
    const auto dot = filename.find_last_of(L'.');
    // Keep leading-dot names (e.g. ".gitignore") as stem-only.
    if (dot != std::wstring::npos && dot > 0) {
        stem = filename.substr(0, dot);
        extension = filename.substr(dot);
    }
    for (unsigned attempt = 1; attempt <= 9999U; ++attempt) {
        std::wstring candidate = directory;
        candidate.append(stem);
        candidate.append(L" (");
        candidate.append(std::to_wstring(attempt));
        candidate.push_back(L')');
        candidate.append(extension);
        if (!path_exists(candidate)) {
            return base::Result<std::wstring>::success(std::move(candidate));
        }
    }
    return base::Result<std::wstring>::failure(
        {base::ErrorCode::kConflict, "file_restore.rename_exhausted"});
}

[[nodiscard]] base::Result<void> move_staging_into_place(const std::wstring& staging_path,
                                                         const std::wstring& destination,
                                                         const DWORD flags) {
    if (MoveFileExW(staging_path.c_str(), destination.c_str(), flags) != FALSE) {
        return base::Result<void>::success();
    }
    const auto error = GetLastError();
    if (error == ERROR_ALREADY_EXISTS || error == ERROR_FILE_EXISTS) {
        return base::Result<void>::failure(
            {base::ErrorCode::kConflict, "file_restore.target_collision"});
    }
    return base::Result<void>::failure(win32_error(error, "MoveFileExW"));
}

class StagedFileWriter final : public ports::IStagedFileWriter {
  public:
    StagedFileWriter(std::wstring final_path, std::wstring staging_path, UniqueHandle handle,
                     const std::uint64_t logical_size)
        : final_path_(std::move(final_path)), staging_path_(std::move(staging_path)),
          handle_(std::move(handle)), logical_size_(logical_size) {}

    ~StagedFileWriter() override { abort(); }

    base::Result<void> write(const std::uint64_t offset, const std::span<const std::byte> payload,
                             const base::CancellationToken cancellation) override {
        if (published_ || aborted_) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "staged file writer is closed"});
        }
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "staged file write cancelled"});
        }
        LARGE_INTEGER distance{};
        distance.QuadPart = static_cast<LONGLONG>(offset);
        if (SetFilePointerEx(handle_.get(), distance, nullptr, FILE_BEGIN) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "SetFilePointerEx"));
        }
        std::size_t written_total = 0;
        while (written_total < payload.size()) {
            const auto chunk = static_cast<DWORD>((std::min)(
                payload.size() - written_total,
                static_cast<std::size_t>((std::numeric_limits<DWORD>::max)())));
            DWORD written = 0;
            if (WriteFile(handle_.get(), payload.data() + written_total, chunk, &written,
                          nullptr) == FALSE ||
                written != chunk) {
                return base::Result<void>::failure(win32_error(GetLastError(), "WriteFile"));
            }
            written_total += written;
        }
        return base::Result<void>::success();
    }

    base::Result<void>
    set_sparse_ranges(const std::vector<contracts::FileAllocatedRangeDesc>& allocated,
                      const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "sparse setup cancelled"});
        }
        DWORD bytes = 0;
        if (DeviceIoControl(handle_.get(), FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &bytes,
                            nullptr) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "FSCTL_SET_SPARSE"));
        }
        static_cast<void>(allocated);
        return base::Result<void>::success();
    }

    base::Result<void> write_alternate_stream(const contracts::EncodedName& name,
                                              const std::span<const std::byte> payload,
                                              const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "ADS write cancelled"});
        }
        auto validated = detail::validate_component(name);
        if (!validated) {
            return validated;
        }
        std::wstring ads = staging_path_;
        ads.push_back(L':');
        std::wstring piece(name.bytes.size() / 2U, L'\0');
        std::memcpy(piece.data(), name.bytes.data(), name.bytes.size());
        ads.append(piece);
        auto handle = detail::open_path(detail::to_utf16_vector(ads), GENERIC_WRITE, 0, CREATE_ALWAYS,
                                        FILE_ATTRIBUTE_NORMAL);
        if (!handle) {
            return base::Result<void>::failure(handle.error());
        }
        DWORD written = 0;
        if (!payload.empty() &&
            WriteFile(handle.value().get(), payload.data(), static_cast<DWORD>(payload.size()),
                      &written, nullptr) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "WriteFile ADS"));
        }
        return base::Result<void>::success();
    }

    base::Result<void> apply_metadata(const contracts::FileEntryDesc& entry,
                                      const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "metadata apply cancelled"});
        }
        FILETIME creation{};
        FILETIME access{};
        FILETIME write{};
        creation.dwLowDateTime = static_cast<DWORD>(entry.creation_time & 0xFFFFFFFFULL);
        creation.dwHighDateTime = static_cast<DWORD>(entry.creation_time >> 32U);
        access.dwLowDateTime = static_cast<DWORD>(entry.access_time & 0xFFFFFFFFULL);
        access.dwHighDateTime = static_cast<DWORD>(entry.access_time >> 32U);
        write.dwLowDateTime = static_cast<DWORD>(entry.write_time & 0xFFFFFFFFULL);
        write.dwHighDateTime = static_cast<DWORD>(entry.write_time >> 32U);
        if (SetFileTime(handle_.get(), &creation, &access, &write) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "SetFileTime"));
        }
        if (SetFileAttributesW(staging_path_.c_str(), entry.attributes) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "SetFileAttributesW"));
        }
        if (!entry.platform_metadata.empty()) {
            auto security =
                format::file_index::extract_platform_security_descriptor(entry.platform_metadata);
            if (!security) {
                return base::Result<void>::failure(security.error());
            }
            if (!security.value().empty()) {
                // Prefer path-based SetFileSecurityW so SACL/Owner apply with restore privileges.
                auto applied = detail::write_self_relative_security_descriptor(
                    staging_path_, security.value());
                if (!applied) {
                    return applied;
                }
            }
        }
        return base::Result<void>::success();
    }

    base::Result<void> publish(const contracts::FileConflictPolicy policy,
                               const base::CancellationToken cancellation) override {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                {base::ErrorCode::kCancelled, "publish cancelled"});
        }
        if (published_ || aborted_) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "staged file already closed"});
        }
        LARGE_INTEGER size{};
        size.QuadPart = static_cast<LONGLONG>(logical_size_);
        if (SetFilePointerEx(handle_.get(), size, nullptr, FILE_BEGIN) == FALSE ||
            SetEndOfFile(handle_.get()) == FALSE) {
            return base::Result<void>::failure(win32_error(GetLastError(), "SetEndOfFile"));
        }
        handle_.reset();
        // Staging may have received READONLY from archive attributes; clear so rename works.
        clear_readonly_attribute(staging_path_);

        const auto destination_attrs = GetFileAttributesW(final_path_.c_str());
        const bool destination_exists = destination_attrs != INVALID_FILE_ATTRIBUTES;
        if (destination_exists && is_directory_attrs(destination_attrs)) {
            return base::Result<void>::failure(
                {base::ErrorCode::kConflict, "file_restore.target_collision"});
        }

        std::wstring destination = final_path_;
        if (destination_exists) {
            switch (policy) {
            case contracts::FileConflictPolicy::kFail:
                return base::Result<void>::failure(
                    {base::ErrorCode::kConflict, "file_restore.target_collision"});
            case contracts::FileConflictPolicy::kReplace: {
                clear_readonly_attribute(final_path_);
                auto replaced = move_staging_into_place(staging_path_, final_path_,
                                                        MOVEFILE_REPLACE_EXISTING);
                if (replaced) {
                    published_ = true;
                    return base::Result<void>::success();
                }
                // Fallback: delete destination then plain rename (handles stubborn attrs/ACLs).
                clear_readonly_attribute(final_path_);
                if (DeleteFileW(final_path_.c_str()) == FALSE) {
                    return base::Result<void>::failure(replaced.error());
                }
                auto moved = move_staging_into_place(staging_path_, final_path_, 0);
                if (!moved) {
                    return moved;
                }
                published_ = true;
                return base::Result<void>::success();
            }
            case contracts::FileConflictPolicy::kRename: {
                auto renamed = allocate_rename_path(final_path_);
                if (!renamed) {
                    return base::Result<void>::failure(renamed.error());
                }
                destination = std::move(renamed).value();
                break;
            }
            }
        }

        auto moved = move_staging_into_place(staging_path_, destination, 0);
        if (!moved) {
            return moved;
        }
        published_ = true;
        return base::Result<void>::success();
    }

    void abort() noexcept override {
        if (published_ || aborted_) {
            return;
        }
        aborted_ = true;
        handle_.reset();
        DeleteFileW(staging_path_.c_str());
    }

  private:
    std::wstring final_path_;
    std::wstring staging_path_;
    UniqueHandle handle_;
    std::uint64_t logical_size_{0};
    bool published_{false};
    bool aborted_{false};
};

} // namespace

struct WindowsFileTreeSink::Impl final {
    std::vector<std::uint16_t> root_utf16;
    UniqueHandle root_handle;
};

WindowsFileTreeSink::WindowsFileTreeSink(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

WindowsFileTreeSink::~WindowsFileTreeSink() = default;

base::Result<std::unique_ptr<WindowsFileTreeSink>>
WindowsFileTreeSink::open(const WindowsFileTreeSinkOpenRequest& request) {
    if (request.target_root_utf16.empty()) {
        return base::Result<std::unique_ptr<WindowsFileTreeSink>>::failure(
            {base::ErrorCode::kInvalidArgument, "target root is required"});
    }
    auto privileges = detail::enable_file_security_privileges();
    if (!privileges) {
        return base::Result<std::unique_ptr<WindowsFileTreeSink>>::failure(privileges.error());
    }
    auto handle = detail::open_path(request.target_root_utf16, FILE_READ_ATTRIBUTES | FILE_LIST_DIRECTORY,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (!handle) {
        return base::Result<std::unique_ptr<WindowsFileTreeSink>>::failure(handle.error());
    }
    auto fs = detail::ensure_ntfs_or_refs(handle.value());
    if (!fs) {
        return base::Result<std::unique_ptr<WindowsFileTreeSink>>::failure(fs.error());
    }
    auto implementation = std::make_unique<Impl>();
    implementation->root_utf16 = request.target_root_utf16;
    implementation->root_handle = std::move(handle).value();
    return base::Result<std::unique_ptr<WindowsFileTreeSink>>::success(
        std::unique_ptr<WindowsFileTreeSink>(new WindowsFileTreeSink(std::move(implementation))));
}

base::Result<ports::FileSinkCapabilities>
WindowsFileTreeSink::capabilities(const base::CancellationToken cancellation) const {
    if (cancellation.stop_requested()) {
        return base::Result<ports::FileSinkCapabilities>::failure(
            {base::ErrorCode::kCancelled, "capabilities cancelled"});
    }
    ports::FileSinkCapabilities caps;
    // Advertised for Port/preflight shape. Full restore of reparse / hard link / sparse /
    // ADS is deferred (this period: not product-complete; see docs/modules/adapters.md).
    caps.supports_ads = true;
    caps.supports_sparse = true;
    caps.supports_security_descriptor = true;
    caps.supports_reparse = true;
    caps.supports_hard_link = true;
    std::wstring root(implementation_->root_utf16.begin(), implementation_->root_utf16.end());
    // Volume GUID roots require a trailing '\\' for GetDiskFreeSpaceExW (ERROR_INVALID_FUNCTION
    // without it). Try as-is, then with/without the trailing separator.
    auto query_free = [](const std::wstring& path) -> std::optional<std::uint64_t> {
        ULARGE_INTEGER free_bytes{};
        if (GetDiskFreeSpaceExW(path.c_str(), &free_bytes, nullptr, nullptr) != FALSE) {
            return free_bytes.QuadPart;
        }
        return std::nullopt;
    };
    auto free = query_free(root);
    if (!free && !root.empty() && root.back() != L'\\') {
        free = query_free(root + L'\\');
    }
    if (!free && root.size() > 1 && root.back() == L'\\') {
        free = query_free(root.substr(0, root.size() - 1));
    }
    if (!free) {
        return base::Result<ports::FileSinkCapabilities>::failure(
            {base::ErrorCode::kIoFailure, "unable to query target free space"});
    }
    caps.free_bytes = free.value();
    return base::Result<ports::FileSinkCapabilities>::success(caps);
}

base::Result<void>
WindowsFileTreeSink::create_directory(const std::vector<contracts::EncodedName>& relative_components,
                                      const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "create directory cancelled"});
    }
    auto path = detail::join_relative_path(implementation_->root_utf16, relative_components);
    if (!path) {
        return base::Result<void>::failure(path.error());
    }
    if (CreateDirectoryW(path.value().c_str(), nullptr) == FALSE) {
        const auto error = GetLastError();
        if (error != ERROR_ALREADY_EXISTS) {
            return base::Result<void>::failure(win32_error(error, "CreateDirectoryW"));
        }
    }
    return base::Result<void>::success();
}

base::Result<std::unique_ptr<ports::IStagedFileWriter>>
WindowsFileTreeSink::begin_file(const std::vector<contracts::EncodedName>& relative_components,
                                const std::uint64_t logical_size,
                                const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IStagedFileWriter>>::failure(
            {base::ErrorCode::kCancelled, "begin file cancelled"});
    }
    auto final_path = detail::join_relative_path(implementation_->root_utf16, relative_components);
    if (!final_path) {
        return base::Result<std::unique_ptr<ports::IStagedFileWriter>>::failure(final_path.error());
    }
    auto staging = final_path.value() + L".aegra-partial";
    auto handle = detail::open_path(detail::to_utf16_vector(staging), GENERIC_WRITE, 0, CREATE_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN);
    if (!handle) {
        return base::Result<std::unique_ptr<ports::IStagedFileWriter>>::failure(handle.error());
    }
    return base::Result<std::unique_ptr<ports::IStagedFileWriter>>::success(
        std::make_unique<StagedFileWriter>(std::move(final_path).value(), std::move(staging),
                                           std::move(handle).value(), logical_size));
}

base::Result<void>
WindowsFileTreeSink::create_hard_link(const std::vector<contracts::EncodedName>& existing_components,
                                      const std::vector<contracts::EncodedName>& new_components,
                                      const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "hard link cancelled"});
    }
    auto existing = detail::join_relative_path(implementation_->root_utf16, existing_components);
    auto neu = detail::join_relative_path(implementation_->root_utf16, new_components);
    if (!existing || !neu) {
        return base::Result<void>::failure(!existing ? existing.error() : neu.error());
    }
    if (CreateHardLinkW(neu.value().c_str(), existing.value().c_str(), nullptr) == FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "CreateHardLinkW"));
    }
    return base::Result<void>::success();
}

base::Result<void>
WindowsFileTreeSink::create_reparse(const std::vector<contracts::EncodedName>&,
                                    const contracts::FileEntryDesc&,
                                    const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "reparse create cancelled"});
    }
    // Full reparse buffer application is completed when platform_metadata section is wired (F8).
    return base::Result<void>::failure(
        {base::ErrorCode::kInvalidArgument, "file_restore.reparse_not_implemented"});
}

base::Result<void> WindowsFileTreeSink::apply_directory_metadata(
    const std::vector<contracts::EncodedName>& relative_components,
    const contracts::FileEntryDesc& entry, const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            {base::ErrorCode::kCancelled, "directory metadata cancelled"});
    }
    auto path = detail::join_relative_path(implementation_->root_utf16, relative_components);
    if (!path) {
        return base::Result<void>::failure(path.error());
    }
    auto handle = detail::open_path(detail::to_utf16_vector(path.value()), FILE_WRITE_ATTRIBUTES,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS);
    if (!handle) {
        return base::Result<void>::failure(handle.error());
    }
    FILETIME creation{};
    FILETIME access{};
    FILETIME write{};
    creation.dwLowDateTime = static_cast<DWORD>(entry.creation_time & 0xFFFFFFFFULL);
    creation.dwHighDateTime = static_cast<DWORD>(entry.creation_time >> 32U);
    access.dwLowDateTime = static_cast<DWORD>(entry.access_time & 0xFFFFFFFFULL);
    access.dwHighDateTime = static_cast<DWORD>(entry.access_time >> 32U);
    write.dwLowDateTime = static_cast<DWORD>(entry.write_time & 0xFFFFFFFFULL);
    write.dwHighDateTime = static_cast<DWORD>(entry.write_time >> 32U);
    if (SetFileTime(handle.value().get(), &creation, &access, &write) == FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "SetFileTime"));
    }
    if (SetFileAttributesW(path.value().c_str(), entry.attributes) == FALSE) {
        return base::Result<void>::failure(win32_error(GetLastError(), "SetFileAttributesW"));
    }
    if (!entry.platform_metadata.empty()) {
        auto security =
            format::file_index::extract_platform_security_descriptor(entry.platform_metadata);
        if (!security) {
            return base::Result<void>::failure(security.error());
        }
        if (!security.value().empty()) {
            auto applied =
                detail::write_self_relative_security_descriptor(path.value(), security.value());
            if (!applied) {
                return applied;
            }
        }
    }
    return base::Result<void>::success();
}

base::Result<void> WindowsFileTreeSink::flush(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure({base::ErrorCode::kCancelled, "flush cancelled"});
    }
    return base::Result<void>::success();
}

} // namespace aegra::adapters::windows_filesystem
