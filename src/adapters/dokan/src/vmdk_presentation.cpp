#include "aegra/adapters/dokan/vmdk_presentation.h"

#include "cow_backing_store.h"
#include "dokan_file_system.h"
#include "vhdx_disk_image.h"
#include "vmdk_disk_image.h"

#include <dokan/dokan.h>

#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::dokan {
namespace {

using detail::CowBackingStore;
using detail::DokanFileSystem;
using detail::VirtualDiskEntry;
using detail::VmdkDescriptorImage;
using detail::VmdkFlatExtentImage;

constexpr const char* kPresentFailed = "bootcheck.vmdk_present_failed";

base::Error present_error(const base::ErrorCode code) { return base::Error{code, kPresentFailed}; }

bool directory_is_empty(const std::wstring& path) {
    WIN32_FIND_DATAW data{};
    const std::wstring query = path + L"\\*";
    HANDLE find = FindFirstFileW(query.c_str(), &data);
    if (find == INVALID_HANDLE_VALUE) {
        return GetLastError() == ERROR_FILE_NOT_FOUND;
    }

    bool empty = true;
    while (true) {
        if (wcscmp(data.cFileName, L".") != 0 && wcscmp(data.cFileName, L"..") != 0) {
            empty = false;
            break;
        }
        if (!FindNextFileW(find, &data)) {
            empty = GetLastError() == ERROR_NO_MORE_FILES;
            break;
        }
    }
    FindClose(find);
    return empty;
}

bool is_local_ntfs_path(const std::wstring& path) {
    wchar_t volume_path[MAX_PATH]{};
    if (!GetVolumePathNameW(path.c_str(), volume_path, MAX_PATH)) {
        return false;
    }
    wchar_t filesystem[MAX_PATH]{};
    if (!GetVolumeInformationW(volume_path, nullptr, 0, nullptr, nullptr, nullptr, filesystem,
                               MAX_PATH)) {
        return false;
    }
    return _wcsicmp(filesystem, L"NTFS") == 0;
}

bool prepare_mount_directory(const std::filesystem::path& requested, std::wstring& normalized,
                             bool& created) {
    if (requested.empty() || !requested.is_absolute()) {
        return false;
    }
    normalized = requested.lexically_normal().wstring();
    if (normalized.rfind(L"\\\\", 0) == 0 || normalized.size() <= 3) {
        return false;
    }

    DWORD attributes = GetFileAttributesW(normalized.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
        if (!CreateDirectoryW(normalized.c_str(), nullptr)) {
            return false;
        }
        created = true;
        attributes = GetFileAttributesW(normalized.c_str());
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
        return false;
    }
    return directory_is_empty(normalized) && is_local_ntfs_path(normalized);
}

std::wstring join_path(const std::wstring& directory, const wchar_t* leaf) {
    std::wstring result = directory;
    if (!result.empty() && result.back() != L'\\') {
        result.push_back(L'\\');
    }
    result.append(leaf);
    return result;
}

bool wait_for_file(const std::wstring& path, const base::CancellationToken cancellation) {
    constexpr DWORD kAttempts = 100;
    constexpr DWORD kDelayMs = 50;
    for (DWORD attempt = 0; attempt < kAttempts; ++attempt) {
        if (cancellation.stop_requested()) {
            return false;
        }
        const DWORD attributes = GetFileAttributesW(path.c_str());
        if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
            return true;
        }
        Sleep(kDelayMs);
    }
    return false;
}

bool is_nonzero_uuid(const std::array<std::uint8_t, 16>& uuid) noexcept {
    for (const std::uint8_t value : uuid) {
        if (value != 0) {
            return true;
        }
    }
    return false;
}

std::vector<VirtualDiskEntry> make_vmdk_entries(ports::IRandomAccessReader& reader,
                                                const std::uint64_t raw_size,
                                                const VmdkImageIdentity& identity, bool& opened) {
    auto descriptor_backing = std::make_unique<CowBackingStore>();
    auto flat_backing = std::make_unique<CowBackingStore>();
    opened = flat_backing->open_reader(&reader, L"", true);
    if (!opened) {
        return {};
    }

    detail::VmdkDescriptorIdentity descriptor_identity;
    descriptor_identity.cid = identity.cid;
    descriptor_identity.image_uuid = identity.image_uuid;
    descriptor_identity.modification_uuid = identity.modification_uuid;
    auto descriptor =
        std::make_unique<VmdkDescriptorImage>(*descriptor_backing, raw_size, descriptor_identity);
    auto flat = std::make_unique<VmdkFlatExtentImage>(*flat_backing);
    descriptor->rebuild();
    flat->rebuild();

    std::vector<VirtualDiskEntry> entries;
    entries.push_back(VirtualDiskEntry{detail::disk_names::kVmdkDescriptor, nullptr,
                                       std::move(descriptor_backing), std::move(descriptor), L""});
    entries.push_back(VirtualDiskEntry{detail::disk_names::kVmdkFlatExtent, &reader,
                                       std::move(flat_backing), std::move(flat), L""});
    return entries;
}

} // namespace

struct ReadOnlyVmdkPresentation::Impl final {
    ~Impl() {
        if (filesystem) {
            try {
                filesystem->close();
            } catch (...) {
            }
            filesystem.reset();
        }
        if (created_mount_directory) {
            RemoveDirectoryW(mount_directory.c_str());
        }
    }

    VmdkPresentationInfo info;
    std::wstring mount_directory;
    std::unique_ptr<DokanFileSystem> filesystem;
    bool created_mount_directory{false};
};

ReadOnlyVmdkPresentation::ReadOnlyVmdkPresentation(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ReadOnlyVmdkPresentation::~ReadOnlyVmdkPresentation() { close(); }

base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>> ReadOnlyVmdkPresentation::create(
    ports::IRandomAccessReader& reader, const std::filesystem::path& mount_directory,
    const VmdkImageIdentity& identity, const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(base::ErrorCode::kCancelled));
    }
    const std::uint64_t raw_size = reader.size_bytes();
    if (raw_size == 0 || identity.cid == 0xFFFFFFFF || !is_nonzero_uuid(identity.image_uuid) ||
        !is_nonzero_uuid(identity.modification_uuid) ||
        raw_size > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)() - 511)) {
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(base::ErrorCode::kInvalidArgument));
    }

    auto impl = std::make_unique<Impl>();
    if (!prepare_mount_directory(mount_directory, impl->mount_directory,
                                 impl->created_mount_directory) ||
        DokanDriverVersion() == 0) {
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }

    bool backing_opened = false;
    auto entries = make_vmdk_entries(reader, raw_size, identity, backing_opened);
    if (!backing_opened) {
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }

    impl->filesystem = std::make_unique<DokanFileSystem>(std::move(entries), true);
    if (impl->filesystem->mount(impl->mount_directory) != DOKAN_SUCCESS) {
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }

    const std::wstring descriptor_path =
        join_path(impl->mount_directory, detail::disk_names::kVmdkDescriptor);
    const std::wstring flat_path =
        join_path(impl->mount_directory, detail::disk_names::kVmdkFlatExtent);
    if (!wait_for_file(descriptor_path, cancellation) || !wait_for_file(flat_path, cancellation)) {
        impl->filesystem->close();
        return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::failure(
            present_error(cancellation.stop_requested() ? base::ErrorCode::kCancelled
                                                        : base::ErrorCode::kIoFailure));
    }

    impl->info.descriptor_path = descriptor_path;
    impl->info.flat_extent_path = flat_path;
    impl->info.virtual_size_bytes = (raw_size + 511) & ~std::uint64_t{511};
    auto presentation =
        std::unique_ptr<ReadOnlyVmdkPresentation>(new ReadOnlyVmdkPresentation(std::move(impl)));
    return base::Result<std::unique_ptr<ReadOnlyVmdkPresentation>>::success(
        std::move(presentation));
}

const VmdkPresentationInfo& ReadOnlyVmdkPresentation::info() const noexcept { return impl_->info; }

bool ReadOnlyVmdkPresentation::active() const noexcept {
    return impl_ != nullptr && impl_->filesystem != nullptr;
}

void ReadOnlyVmdkPresentation::close() noexcept {
    if (!impl_ || !impl_->filesystem) {
        return;
    }
    try {
        impl_->filesystem->close();
    } catch (...) {
    }
    impl_->filesystem.reset();
    if (impl_->created_mount_directory) {
        RemoveDirectoryW(impl_->mount_directory.c_str());
        impl_->created_mount_directory = false;
    }
}

struct ReadOnlyVhdxPresentation::Impl final {
    ~Impl() {
        if (filesystem) {
            try {
                filesystem->close();
            } catch (...) {
            }
            filesystem.reset();
        }
        if (created_mount_directory) {
            RemoveDirectoryW(mount_directory.c_str());
        }
    }

    VhdxPresentationInfo info;
    std::wstring mount_directory;
    std::unique_ptr<DokanFileSystem> filesystem;
    bool created_mount_directory{false};
};

ReadOnlyVhdxPresentation::ReadOnlyVhdxPresentation(std::unique_ptr<Impl> impl)
    : impl_(std::move(impl)) {}

ReadOnlyVhdxPresentation::~ReadOnlyVhdxPresentation() { close(); }

base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>
ReadOnlyVhdxPresentation::create(ports::IRandomAccessReader& reader,
                                 const std::filesystem::path& mount_directory,
                                 const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(base::ErrorCode::kCancelled));
    }
    if (reader.size_bytes() == 0) {
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(base::ErrorCode::kInvalidArgument));
    }

    auto impl = std::make_unique<Impl>();
    if (!prepare_mount_directory(mount_directory, impl->mount_directory,
                                 impl->created_mount_directory) ||
        DokanDriverVersion() == 0) {
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }

    auto backing = std::make_unique<CowBackingStore>();
    if (!backing->open_reader(&reader, L"", true)) {
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }
    auto image = std::make_unique<detail::VhdxDiskImage>(*backing);
    image->rebuild();
    const auto file_size = image->file_size();

    std::vector<VirtualDiskEntry> entries;
    entries.push_back(VirtualDiskEntry{detail::disk_names::kVhdx, &reader, std::move(backing),
                                       std::move(image), L""});
    impl->filesystem = std::make_unique<DokanFileSystem>(std::move(entries), true);
    if (impl->filesystem->mount(impl->mount_directory) != DOKAN_SUCCESS) {
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(base::ErrorCode::kIoFailure));
    }

    const std::wstring vhdx_path = join_path(impl->mount_directory, detail::disk_names::kVhdx);
    if (!wait_for_file(vhdx_path, cancellation)) {
        impl->filesystem->close();
        return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::failure(
            present_error(cancellation.stop_requested() ? base::ErrorCode::kCancelled
                                                        : base::ErrorCode::kIoFailure));
    }

    impl->info.vhdx_path = vhdx_path;
    impl->info.virtual_size_bytes = file_size;
    auto presentation =
        std::unique_ptr<ReadOnlyVhdxPresentation>(new ReadOnlyVhdxPresentation(std::move(impl)));
    return base::Result<std::unique_ptr<ReadOnlyVhdxPresentation>>::success(
        std::move(presentation));
}

const VhdxPresentationInfo& ReadOnlyVhdxPresentation::info() const noexcept { return impl_->info; }

bool ReadOnlyVhdxPresentation::active() const noexcept {
    return impl_ != nullptr && impl_->filesystem != nullptr;
}

void ReadOnlyVhdxPresentation::close() noexcept {
    if (!impl_ || !impl_->filesystem) {
        return;
    }
    try {
        impl_->filesystem->close();
    } catch (...) {
    }
    impl_->filesystem.reset();
    if (impl_->created_mount_directory) {
        RemoveDirectoryW(impl_->mount_directory.c_str());
        impl_->created_mount_directory = false;
    }
}

} // namespace aegra::adapters::dokan
