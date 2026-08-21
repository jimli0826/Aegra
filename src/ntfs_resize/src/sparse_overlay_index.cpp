#include "sparse_overlay_index.h"

namespace aegra::ntfs_resize::detail {

void SparseOverlayIndex::mark_written(const std::uint64_t offset, const std::uint64_t size) {
    if (size == 0) {
        return;
    }
    const auto first = overlay_page_index(offset);
    const auto last_byte = offset + size - 1U;
    const auto last = overlay_page_index(last_byte);
    for (auto page = first; page <= last; ++page) {
        pages_.insert(page);
        if (page == last) {
            break;
        }
    }
}

bool SparseOverlayIndex::contains_page(const std::uint64_t page_index) const noexcept {
    return pages_.find(page_index) != pages_.end();
}

bool SparseOverlayIndex::intersects(const std::uint64_t offset,
                                    const std::uint64_t size) const noexcept {
    if (size == 0 || pages_.empty()) {
        return false;
    }
    const auto first = overlay_page_index(offset);
    const auto last_byte = offset + size - 1U;
    const auto last = overlay_page_index(last_byte);
    for (auto page = first; page <= last; ++page) {
        if (contains_page(page)) {
            return true;
        }
        if (page == last) {
            break;
        }
    }
    return false;
}

} // namespace aegra::ntfs_resize::detail
