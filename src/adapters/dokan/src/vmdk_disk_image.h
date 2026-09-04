#pragma once

#include "disk_image.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace aegra::adapters::dokan::detail {

struct VmdkDescriptorIdentity final {
    std::uint32_t cid{0};
    std::array<std::uint8_t, 16> image_uuid{};
    std::array<std::uint8_t, 16> modification_uuid{};
};

class VmdkDescriptorImage final : public DiskImage {
  public:
    VmdkDescriptorImage(CowBackingStore& backing, std::uint64_t raw_size_bytes,
                        VmdkDescriptorIdentity identity);

    [[nodiscard]] const wchar_t* file_name() const override { return disk_names::kVmdkDescriptor; }

  protected:
    void rebuild_locked() override;
    [[nodiscard]] NTSTATUS read_locked(std::uint64_t offset, void* buffer, DWORD buffer_len,
                                       LPDWORD bytes_read) override;
    [[nodiscard]] NTSTATUS write_locked(std::uint64_t offset, const void* buffer,
                                        DWORD bytes_to_write, LPDWORD bytes_written) override;
    [[nodiscard]] std::uint64_t file_size_locked() const override;
    [[nodiscard]] std::uint64_t data_offset_locked() const override;
    [[nodiscard]] std::uint64_t unit_size_locked() const override;

  private:
    std::uint64_t raw_size_bytes_{0};
    VmdkDescriptorIdentity identity_{};
    std::vector<std::uint8_t> descriptor_;
};

class VmdkFlatExtentImage final : public DiskImage {
  public:
    explicit VmdkFlatExtentImage(CowBackingStore& backing) : DiskImage(backing) {}

    [[nodiscard]] const wchar_t* file_name() const override { return disk_names::kVmdkFlatExtent; }

  protected:
    void rebuild_locked() override;
    [[nodiscard]] NTSTATUS read_locked(std::uint64_t offset, void* buffer, DWORD buffer_len,
                                       LPDWORD bytes_read) override;
    [[nodiscard]] NTSTATUS write_locked(std::uint64_t offset, const void* buffer,
                                        DWORD bytes_to_write, LPDWORD bytes_written) override;
    [[nodiscard]] std::uint64_t file_size_locked() const override { return extent_size_bytes_; }
    [[nodiscard]] std::uint64_t data_offset_locked() const override { return 0; }
    [[nodiscard]] std::uint64_t unit_size_locked() const override { return 512; }

  private:
    std::uint64_t extent_size_bytes_{0};
};

} // namespace aegra::adapters::dokan::detail
