#pragma once

#include "disk_image.h"

#include <cstdint>
#include <vector>

namespace aegra::adapters::dokan::detail {

class VhdxDiskImage final : public DiskImage {
  public:
    explicit VhdxDiskImage(CowBackingStore& backing) : DiskImage(backing) {}

    [[nodiscard]] const wchar_t* file_name() const override {
        return disk_names::kVhdx;
    }

  protected:
    void rebuild_locked() override;
    [[nodiscard]] NTSTATUS read_locked(std::uint64_t offset, void* buffer,
                                       DWORD buffer_len,
                                       LPDWORD bytes_read) override;
    [[nodiscard]] NTSTATUS write_locked(std::uint64_t offset, const void* buffer,
                                        DWORD bytes_to_write,
                                        LPDWORD bytes_written) override;
    [[nodiscard]] std::uint64_t file_size_locked() const override {
        return layout_.vhdx_file_size;
    }
    [[nodiscard]] std::uint64_t data_offset_locked() const override {
        return layout_.data_offset;
    }
    [[nodiscard]] std::uint64_t unit_size_locked() const override {
        return layout_.block_size;
    }

  private:
    struct Layout {
        std::uint64_t raw_data_size{0};
        std::uint64_t virtual_disk_size{0};
        std::uint64_t block_size{0};
        std::uint64_t block_count{0};
        std::uint64_t last_block_size{0};

        std::uint64_t header_offset{0};
        std::uint64_t log_offset{0};
        std::uint64_t metadata_offset{0};
        std::uint64_t bat_offset{0};
        std::uint64_t data_offset{0};

        std::uint64_t chunk_ratio{0};
        std::uint64_t sector_bitmap_block_count{0};
        std::uint64_t bat_entry_count{0};
        std::uint64_t bat_size{0};
        std::uint64_t bat_region_size{0};
        std::uint64_t metadata_region_size{0};

        std::uint64_t vhdx_file_size{0};
    };

    enum class Region { kHeader, kLog, kMetadata, kBat, kData, kOutOfRange };

    void compute_layout();
    void build_header_region();
    void build_log_region();
    void build_bat_data();

    [[nodiscard]] Region classify_offset(std::uint64_t vhdx_offset) const;
    [[nodiscard]] bool to_raw_offset(std::uint64_t vhdx_offset,
                                     std::uint64_t* raw_offset,
                                     std::uint64_t* remaining) const;

    Layout layout_{};
    std::vector<std::uint8_t> header_region_;
    std::vector<std::uint8_t> log_region_;
    std::vector<std::uint8_t> metadata_region_;
    std::vector<std::uint8_t> bat_region_;
};

} // namespace aegra::adapters::dokan::detail
