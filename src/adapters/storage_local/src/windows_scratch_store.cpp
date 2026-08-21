#include "aegra/adapters/storage_local/windows_scratch_store.h"

#include "windows_scratch_internal.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace aegra::adapters::storage_local {
namespace {

using detail::kScratchPageSize;
using detail::local_error;
using detail::UniqueHandle;

[[nodiscard]] base::Result<void>
require_scratch_space(const std::wstring& path, const std::uint64_t maximum_allocation_bytes) {
    const auto slash = path.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch parent directory is missing"));
    }
    ULARGE_INTEGER available{};
    const auto parent = path.substr(0, slash + 1U);
    if (!GetDiskFreeSpaceExW(parent.c_str(), &available, nullptr, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "GetDiskFreeSpaceExW scratch"));
    }
    constexpr std::uint64_t kIndexReserveBytes = 2U * 1024U * 1024U;
    if (maximum_allocation_bytes > available.QuadPart ||
        kIndexReserveBytes > available.QuadPart - maximum_allocation_bytes) {
        return base::Result<void>::failure(local_error(
            base::ErrorCode::kInsufficientSpace,
            "restore.shrink_scratch_insufficient: scratch free space is insufficient"));
    }
    return base::Result<void>::success();
}

class WindowsScratchStore final : public ports::IScratchStore {
  public:
    WindowsScratchStore(UniqueHandle file_handle, std::wstring path, std::wstring idx_path,
                        std::uint64_t logical_size_bytes, std::uint64_t maximum_allocation_bytes,
                        std::uint64_t memory_budget_bytes)
        : file_handle_(std::move(file_handle)), path_(std::move(path)),
          logical_size_bytes_(logical_size_bytes),
          maximum_allocation_bytes_(maximum_allocation_bytes),
          index_(std::move(idx_path), static_cast<std::size_t>(memory_budget_bytes)) {}

    ~WindowsScratchStore() override {
        std::lock_guard lock(mutex_);
        discard_unlocked();
    }

    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override {
        return logical_size_bytes_;
    }

    [[nodiscard]] std::uint64_t allocated_bytes() const noexcept override {
        std::lock_guard lock(mutex_);
        return index_.written_page_count() * static_cast<std::uint64_t>(kScratchPageSize);
    }

    [[nodiscard]] std::uint64_t maximum_allocation_bytes() const noexcept override {
        return maximum_allocation_bytes_;
    }

    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void> write_at(std::uint64_t offset, std::span<const std::byte> source,
                                              base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void> flush(base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void> verify_range(std::uint64_t offset, std::uint64_t length,
                                                  base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<void> close_and_discard() override;

    [[nodiscard]] base::Result<void> initialize() { return index_.initialize(); }

  private:
    [[nodiscard]] base::Result<void> ensure_open_unlocked() const;
    [[nodiscard]] base::Result<void>
    validate_range_unlocked(std::uint64_t offset, std::uint64_t length) const;
    [[nodiscard]] base::Result<void>
    load_page_unlocked(std::uint64_t page_index, std::span<std::byte, kScratchPageSize> page);
    [[nodiscard]] base::Result<void>
    store_page_unlocked(std::uint64_t page_index, std::span<const std::byte, kScratchPageSize> page);
    void discard_unlocked() noexcept;

    mutable std::mutex mutex_;
    UniqueHandle file_handle_;
    std::wstring path_;
    std::uint64_t logical_size_bytes_{0};
    std::uint64_t maximum_allocation_bytes_{0};
    detail::ScratchPageIndex index_;
    bool closed_{false};
};

base::Result<void> WindowsScratchStore::ensure_open_unlocked() const {
    if (closed_ || !file_handle_.valid()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kConflict, "scratch store is closed"));
    }
    return base::Result<void>::success();
}

base::Result<void>
WindowsScratchStore::validate_range_unlocked(const std::uint64_t offset,
                                             const std::uint64_t length) const {
    if (length > 0 && offset > (std::numeric_limits<std::uint64_t>::max)() - length) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch range overflows"));
    }
    if (offset + length > logical_size_bytes_) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch range exceeds logical size"));
    }
    return base::Result<void>::success();
}

base::Result<void>
WindowsScratchStore::load_page_unlocked(const std::uint64_t page_index,
                                        const std::span<std::byte, kScratchPageSize> page) {
    auto present = index_.contains(page_index);
    if (!present) {
        return base::Result<void>::failure(present.error());
    }
    if (!present.value()) {
        std::memset(page.data(), 0, page.size());
        return base::Result<void>::success();
    }
    const auto offset = page_index * static_cast<std::uint64_t>(kScratchPageSize);
    auto read = detail::scratch_read_exact(file_handle_.get(), offset, page);
    if (!read) {
        return read;
    }
    auto expected = index_.get_crc(page_index);
    if (!expected) {
        return base::Result<void>::failure(expected.error());
    }
    if (detail::scratch_crc32_ieee(page) != expected.value()) {
        return base::Result<void>::failure(
            local_error(base::ErrorCode::kCorruptData, "scratch page CRC mismatch"));
    }
    return base::Result<void>::success();
}

base::Result<void>
WindowsScratchStore::store_page_unlocked(const std::uint64_t page_index,
                                         const std::span<const std::byte, kScratchPageSize> page) {
    auto present = index_.contains(page_index);
    if (!present) {
        return base::Result<void>::failure(present.error());
    }
    if (!present.value()) {
        const auto next_allocated =
            (index_.written_page_count() + 1U) * static_cast<std::uint64_t>(kScratchPageSize);
        if (next_allocated > maximum_allocation_bytes_) {
            return base::Result<void>::failure(
                local_error(base::ErrorCode::kInsufficientSpace,
                            "restore.shrink_scratch_insufficient: scratch quota exceeded"));
        }
    }
    const auto offset = page_index * static_cast<std::uint64_t>(kScratchPageSize);
    auto written = detail::scratch_write_exact(file_handle_.get(), offset, page);
    if (!written) {
        return written;
    }
    return index_.put_crc(page_index, detail::scratch_crc32_ieee(page));
}

void WindowsScratchStore::discard_unlocked() noexcept {
    if (closed_) {
        return;
    }
    closed_ = true;
    file_handle_.reset();
    index_.close();
    if (!path_.empty()) {
        DeleteFileW(path_.c_str());
    }
    if (index_.owns_idx_file() && !index_.idx_path().empty()) {
        DeleteFileW(index_.idx_path().c_str());
    }
    if (index_.owns_temp_file() && !index_.idx_path().empty()) {
        DeleteFileW((index_.idx_path() + L".tmp").c_str());
    }
}

base::Result<std::size_t>
WindowsScratchStore::read_at(const std::uint64_t offset, const std::span<std::byte> destination,
                             const base::CancellationToken cancellation) {
    std::lock_guard lock(mutex_);
    auto open = ensure_open_unlocked();
    auto active = open ? detail::check_cancelled(cancellation) : open;
    auto range =
        active ? validate_range_unlocked(offset, destination.size()) : active;
    if (!range) {
        return base::Result<std::size_t>::failure(range.error());
    }
    if (destination.empty()) {
        return base::Result<std::size_t>::success(0);
    }

    std::byte page_storage[kScratchPageSize];
    const std::span<std::byte, kScratchPageSize> page(page_storage);
    std::size_t done = 0;
    while (done < destination.size()) {
        active = detail::check_cancelled(cancellation);
        if (!active) {
            return base::Result<std::size_t>::failure(active.error());
        }
        const auto absolute = offset + done;
        const auto page_index = absolute / kScratchPageSize;
        const auto page_offset = static_cast<std::size_t>(absolute % kScratchPageSize);
        const auto chunk =
            (std::min)(destination.size() - done, static_cast<std::size_t>(kScratchPageSize) - page_offset);
        auto loaded = load_page_unlocked(page_index, page);
        if (!loaded) {
            return base::Result<std::size_t>::failure(loaded.error());
        }
        std::memcpy(destination.data() + done, page.data() + page_offset, chunk);
        done += chunk;
    }
    return base::Result<std::size_t>::success(done);
}

base::Result<void> WindowsScratchStore::write_at(const std::uint64_t offset,
                                                 const std::span<const std::byte> source,
                                                 const base::CancellationToken cancellation) {
    std::lock_guard lock(mutex_);
    auto open = ensure_open_unlocked();
    auto active = open ? detail::check_cancelled(cancellation) : open;
    auto range = active ? validate_range_unlocked(offset, source.size()) : active;
    if (!range) {
        return range;
    }
    if (source.empty()) {
        return base::Result<void>::success();
    }

    std::byte page_storage[kScratchPageSize];
    const std::span<std::byte, kScratchPageSize> page(page_storage);
    std::size_t done = 0;
    while (done < source.size()) {
        active = detail::check_cancelled(cancellation);
        if (!active) {
            return active;
        }
        const auto absolute = offset + done;
        const auto page_index = absolute / kScratchPageSize;
        const auto page_offset = static_cast<std::size_t>(absolute % kScratchPageSize);
        const auto chunk =
            (std::min)(source.size() - done, static_cast<std::size_t>(kScratchPageSize) - page_offset);
        auto loaded = load_page_unlocked(page_index, page);
        if (!loaded) {
            return loaded;
        }
        std::memcpy(page.data() + page_offset, source.data() + done, chunk);
        auto stored = store_page_unlocked(page_index, page);
        if (!stored) {
            return stored;
        }
        done += chunk;
    }
    return base::Result<void>::success();
}

base::Result<void> WindowsScratchStore::flush(const base::CancellationToken cancellation) {
    std::lock_guard lock(mutex_);
    auto open = ensure_open_unlocked();
    auto active = open ? detail::check_cancelled(cancellation) : open;
    if (!active) {
        return active;
    }
    if (!FlushFileBuffers(file_handle_.get())) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "FlushFileBuffers scratch"));
    }
    return index_.flush();
}

base::Result<void> WindowsScratchStore::verify_range(const std::uint64_t offset,
                                                     const std::uint64_t length,
                                                     const base::CancellationToken cancellation) {
    std::lock_guard lock(mutex_);
    auto open = ensure_open_unlocked();
    auto active = open ? detail::check_cancelled(cancellation) : open;
    auto range = active ? validate_range_unlocked(offset, length) : active;
    if (!range) {
        return range;
    }
    if (length == 0) {
        return base::Result<void>::success();
    }
    const auto first_page = offset / kScratchPageSize;
    const auto last_byte = offset + length - 1U;
    const auto last_page = last_byte / kScratchPageSize;
    auto pages = index_.written_pages_in_range(first_page, last_page);
    if (!pages) {
        return base::Result<void>::failure(pages.error());
    }
    std::byte page_storage[kScratchPageSize];
    const std::span<std::byte, kScratchPageSize> page(page_storage);
    for (const auto page_index : pages.value()) {
        active = detail::check_cancelled(cancellation);
        if (!active) {
            return active;
        }
        auto loaded = load_page_unlocked(page_index, page);
        if (!loaded) {
            return loaded;
        }
    }
    return base::Result<void>::success();
}

base::Result<void> WindowsScratchStore::close_and_discard() {
    std::lock_guard lock(mutex_);
    discard_unlocked();
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::unique_ptr<ports::IScratchStore>>
open_windows_scratch_store(const ports::ScratchStoreOpenRequest& request,
                           const base::CancellationToken cancellation) {
    auto active = detail::check_cancelled(cancellation);
    if (!active) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(active.error());
    }
    if (request.logical_size_bytes == 0 || request.maximum_allocation_bytes == 0 ||
        request.memory_budget_bytes == 0 || request.logical_size_bytes >
        static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(
            local_error(base::ErrorCode::kInvalidArgument, "scratch logical size is invalid"));
    }
    auto path = detail::scratch_resolve_absolute_local_path(request.path_utf8);
    if (!path) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(path.error());
    }
    auto parent = detail::scratch_require_parent_directory(path.value());
    if (!parent) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(parent.error());
    }
    auto volume =
        detail::scratch_reject_forbidden_volume(path.value(), request.forbidden_volume_guid_utf8);
    if (!volume) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(volume.error());
    }
    auto space = require_scratch_space(path.value(), request.maximum_allocation_bytes);
    if (!space) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(space.error());
    }
    active = detail::check_cancelled(cancellation);
    if (!active) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(active.error());
    }

    const std::wstring idx_path = path.value() + L".idx";
    auto handle = detail::scratch_create_sparse_file(path.value(), request.logical_size_bytes);
    if (!handle) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(handle.error());
    }
    auto store = std::make_unique<WindowsScratchStore>(
        std::move(handle).value(), std::move(path).value(), idx_path, request.logical_size_bytes,
        request.maximum_allocation_bytes, request.memory_budget_bytes);
    auto initialized = store->initialize();
    if (!initialized) {
        return base::Result<std::unique_ptr<ports::IScratchStore>>::failure(initialized.error());
    }
    return base::Result<std::unique_ptr<ports::IScratchStore>>::success(std::move(store));
}

} // namespace

base::Result<std::unique_ptr<ports::IScratchStore>>
WindowsScratchStoreFactory::open(const ports::ScratchStoreOpenRequest& request,
                                 const base::CancellationToken cancellation) {
    return open_windows_scratch_store(request, cancellation);
}

} // namespace aegra::adapters::storage_local
