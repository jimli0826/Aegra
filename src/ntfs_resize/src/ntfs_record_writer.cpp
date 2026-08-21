#include "ntfs_record_writer.h"

#include "ntfs_shrink_errors.h"

#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/fixup.h"
#include "aegra/ntfs_core/layout_read.h"

#include <algorithm>
#include <cstring>
#include <utility>

namespace aegra::ntfs_resize::detail {
namespace {

class CompositeReaderAdapter final : public ports::IRandomAccessReader {
  public:
    explicit CompositeReaderAdapter(CompositeNtfsBlockDevice& device) noexcept : device_(&device) {}

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
        return device_->source_logical_size_bytes();
    }

    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override {
        return device_->read_at(offset, destination, cancellation);
    }

  private:
    CompositeNtfsBlockDevice* device_;
};

[[nodiscard]] base::Result<std::uint64_t>
record_file_offset(const ntfs_core::BootGeometry& geometry, const std::uint64_t record_number) {
    std::uint64_t offset = 0;
    if (!ntfs_core::checked_mul_u64(record_number, geometry.bytes_per_mft_record, offset)) {
        return shrink_fail<std::uint64_t>(base::ErrorCode::kCorruptData,
                                          "restore.shrink_plan_corrupt");
    }
    return base::Result<std::uint64_t>::success(offset);
}

} // namespace

MftRecordStore::MftRecordStore(CompositeNtfsBlockDevice& device, ntfs_core::BootGeometry geometry,
                               ntfs_core::AttributeValue mft_data)
    : device_(&device), geometry_(std::move(geometry)), mft_data_(std::move(mft_data)) {}

MftRecordStore::MftRecordStore(ports::IRandomAccessReader& reader,
                               ntfs_core::BootGeometry geometry,
                               ntfs_core::AttributeValue mft_data)
    : device_(nullptr), reader_(&reader), geometry_(std::move(geometry)),
      mft_data_(std::move(mft_data)) {}

base::Result<std::vector<std::byte>>
MftRecordStore::read_record_bytes(const std::uint64_t record_number,
                                  const base::CancellationToken cancellation) {
    auto file_offset = record_file_offset(geometry_, record_number);
    if (!file_offset) {
        return base::Result<std::vector<std::byte>>::failure(file_offset.error());
    }
    std::vector<std::byte> record(geometry_.bytes_per_mft_record);
    base::Result<std::size_t> n = base::Result<std::size_t>::failure(
        {base::ErrorCode::kInternal, "ntfs_resize.record_reader_missing"});
    if (reader_ != nullptr) {
        n = ntfs_core::read_from_attribute(
            *reader_, geometry_, mft_data_, ntfs_core::ByteOffset{file_offset.value()},
            std::span<std::byte>(record), cancellation);
    } else {
        CompositeReaderAdapter reader(*device_);
        n = ntfs_core::read_from_attribute(
            reader, geometry_, mft_data_, ntfs_core::ByteOffset{file_offset.value()},
            std::span<std::byte>(record), cancellation);
    }
    if (!n) {
        return base::Result<std::vector<std::byte>>::failure(n.error());
    }
    if (n.value() != record.size()) {
        return shrink_fail<std::vector<std::byte>>(base::ErrorCode::kCorruptData,
                                                   "restore.shrink_unsupported_layout");
    }
    return base::Result<std::vector<std::byte>>::success(std::move(record));
}

base::Result<void>
MftRecordStore::write_record_bytes(const std::uint64_t record_number,
                                   const std::span<std::byte> record_bytes,
                                   const base::CancellationToken cancellation) {
    if (record_bytes.size() != geometry_.bytes_per_mft_record) {
        return shrink_fail_void(base::ErrorCode::kInvalidArgument, "restore.shrink_plan_corrupt");
    }
    if (device_ == nullptr) {
        return shrink_fail_void(base::ErrorCode::kConflict,
                                "ntfs_resize.read_only_record_store");
    }
    const auto usa_offset = ntfs_core::read_u16(record_bytes, 4);
    const auto usa_count = ntfs_core::read_u16(record_bytes, 6);
    auto sealed =
        ntfs_core::seal_fixup(record_bytes, geometry_.bytes_per_sector, usa_offset, usa_count);
    if (!sealed) {
        return sealed;
    }

    auto file_offset = record_file_offset(geometry_, record_number);
    if (!file_offset) {
        return base::Result<void>::failure(file_offset.error());
    }

    // Write through $MFT data runs by expanding VCN→LCN and writing each piece.
    std::uint64_t remaining = record_bytes.size();
    std::uint64_t position = 0;
    auto vcn = file_offset.value() / geometry_.bytes_per_cluster;
    auto cluster_offset = file_offset.value() % geometry_.bytes_per_cluster;
    for (const auto& run : mft_data_.runs) {
        if (remaining == 0) {
            break;
        }
        if (run.sparse) {
            return shrink_fail_void(base::ErrorCode::kUnsupportedVersion,
                                    "restore.shrink_unsupported_layout");
        }
        const auto run_end_vcn = run.first_vcn.value + run.cluster_count.value;
        if (vcn >= run_end_vcn) {
            continue;
        }
        if (vcn < run.first_vcn.value) {
            return shrink_fail_void(base::ErrorCode::kCorruptData,
                                    "restore.shrink_unsupported_layout");
        }
        const auto clusters_into_run = vcn - run.first_vcn.value;
        std::uint64_t lcn_offset = 0;
        if (!ntfs_core::checked_mul_u64(run.first_lcn.value + clusters_into_run,
                                        geometry_.bytes_per_cluster, lcn_offset) ||
            !ntfs_core::checked_add_u64(lcn_offset, cluster_offset, lcn_offset)) {
            return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt");
        }
        const auto run_bytes_left =
            (run_end_vcn - vcn) * geometry_.bytes_per_cluster - cluster_offset;
        const auto piece = static_cast<std::size_t>((std::min)(remaining, run_bytes_left));
        auto written =
            device_->write_at(lcn_offset, record_bytes.subspan(static_cast<std::size_t>(position), piece),
                              cancellation);
        if (!written) {
            return written;
        }
        position += piece;
        remaining -= piece;
        vcn = (file_offset.value() + position) / geometry_.bytes_per_cluster;
        cluster_offset = (file_offset.value() + position) % geometry_.bytes_per_cluster;
    }
    if (remaining != 0) {
        return shrink_fail_void(base::ErrorCode::kCorruptData, "restore.shrink_unsupported_layout");
    }
    auto flushed = device_->flush(cancellation);
    if (!flushed) {
        return flushed;
    }
    auto readback = read_record_bytes(record_number, cancellation);
    if (!readback) {
        return base::Result<void>::failure(readback.error());
    }
    // Compare after apply_fixup on both sides would be ideal; sealed on-disk form must match.
    if (readback.value().size() != record_bytes.size() ||
        std::memcmp(readback.value().data(), record_bytes.data(), record_bytes.size()) != 0) {
        return shrink_fail_void(base::ErrorCode::kIoFailure, "restore.shrink_target_incomplete");
    }
    return base::Result<void>::success();
}

} // namespace aegra::ntfs_resize::detail
