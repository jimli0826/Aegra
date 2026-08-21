#pragma once

#include <cstdint>
#include <unordered_set>

namespace aegra::ntfs_resize::detail {

/// Page size for overlay presence tracking; matches Windows scratch store pages.
inline constexpr std::uint64_t kOverlayPageSizeBytes = 65536;

/// In-memory page presence index for composite overlay reads.
/// Keys are page indices; lookups are O(1) average, never a linear scan of write records.
class SparseOverlayIndex final {
  public:
    SparseOverlayIndex() = default;

    /// Marks every page intersecting the half-open range [offset, offset+size).
    void mark_written(std::uint64_t offset, std::uint64_t size);

    [[nodiscard]] bool contains_page(std::uint64_t page_index) const noexcept;

    /// True if any page intersecting [offset, offset+size) is marked written.
    [[nodiscard]] bool intersects(std::uint64_t offset, std::uint64_t size) const noexcept;

    [[nodiscard]] std::size_t written_page_count() const noexcept {
        return pages_.size();
    }

  private:
    std::unordered_set<std::uint64_t> pages_{};
};

[[nodiscard]] inline std::uint64_t overlay_page_index(const std::uint64_t offset) noexcept {
    return offset / kOverlayPageSizeBytes;
}

} // namespace aegra::ntfs_resize::detail
