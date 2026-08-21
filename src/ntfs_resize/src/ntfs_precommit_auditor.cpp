#include "aegra/ntfs_resize/ntfs_precommit_auditor.h"

#include "block_device_byte_io.h"
#include "ntfs_record_writer.h"
#include "ntfs_shrink_errors.h"
#include "ntfs_volume_view.h"

#include "aegra/ntfs_core/attribute_list.h"
#include "aegra/ntfs_core/binary.h"
#include "aegra/ntfs_core/bitmap.h"
#include "aegra/ntfs_core/layout_read.h"
#include "aegra/ntfs_core/mft_record.h"
#include "aegra/ntfs_core/runlist.h"

#include <algorithm>
#include <cstring>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace aegra::ntfs_resize {
namespace {

class TargetReaderAdapter final : public ports::IRandomAccessReader {
  public:
    explicit TargetReaderAdapter(ports::IRandomAccessBlockDevice& device) noexcept
        : device_(&device) {}
    [[nodiscard]] std::uint64_t size_bytes() const noexcept override {
        return device_->geometry().capacity_bytes;
    }
    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override {
        return detail::read_block_device_bytes(*device_, offset, destination, cancellation);
    }

  private:
    ports::IRandomAccessBlockDevice* device_;
};

void add_failure(PrecommitAuditReport& report, std::string code, std::string detail = {}) {
    report.passed = false;
    report.failure_codes.push_back(std::move(code));
    report.failure_details.push_back(std::move(detail));
}

[[nodiscard]] bool record_bytes_zeroed(const std::span<const std::byte> bytes) {
    return std::all_of(bytes.begin(), bytes.end(),
                       [](const std::byte value) { return value == std::byte{0}; });
}

[[nodiscard]] std::string describe_record_bytes(const std::span<const std::byte> bytes) {
    if (record_bytes_zeroed(bytes)) {
        return " zeroed=true";
    }
    if (bytes.size() >= 4 && std::memcmp(bytes.data(), "FILE", 4) != 0) {
        return " signature_missing=true";
    }
    return {};
}

[[nodiscard]] base::Result<void>
validate_request(const PrecommitAuditRequest& request) {
    if (request.plan == nullptr || request.device == nullptr || request.target == nullptr) {
        return detail::shrink_fail_void(base::ErrorCode::kInvalidArgument,
                                        "restore.shrink_plan_corrupt");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<ntfs_core::BootGeometry> candidate_geometry(const ShrinkPlan& plan) {
    auto geometry = plan.source_ntfs_geometry();
    geometry.total_clusters = plan.new_total_cluster_count();
    if (!ntfs_core::checked_mul_u64(plan.new_total_sector_count(), geometry.bytes_per_sector,
                                    geometry.volume_size_bytes.value)) {
        return detail::shrink_fail<ntfs_core::BootGeometry>(
            base::ErrorCode::kCorruptData, "restore.shrink_plan_corrupt");
    }
    geometry.mft_start_lcn.value = plan.new_mft_start_lcn();
    geometry.mft_mirror_start_lcn.value = plan.new_mft_mirror_start_lcn();
    return base::Result<ntfs_core::BootGeometry>::success(geometry);
}

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
load_target_mft_data(TargetReaderAdapter& reader, const ntfs_core::BootGeometry& geometry,
                     const base::CancellationToken cancellation) {
    std::uint64_t offset = 0;
    if (!ntfs_core::checked_mul_u64(geometry.mft_start_lcn.value, geometry.bytes_per_cluster,
                                    offset) ||
        offset > reader.size_bytes() || geometry.bytes_per_mft_record > reader.size_bytes() - offset) {
        return detail::shrink_fail<ntfs_core::AttributeValue>(
            base::ErrorCode::kCorruptData, "ntfs_resize.audit_mft_unreadable");
    }
    std::vector<std::byte> raw(geometry.bytes_per_mft_record);
    auto read = reader.read_at(offset, raw, cancellation);
    if (!read || read.value() != raw.size()) {
        return detail::shrink_fail<ntfs_core::AttributeValue>(
            base::ErrorCode::kIoFailure, "ntfs_resize.audit_mft_unreadable");
    }
    auto parsed = ntfs_core::parse_mft_record_bytes(raw, geometry.bytes_per_sector, 0);
    if (!parsed) {
        return base::Result<ntfs_core::AttributeValue>::failure(parsed.error());
    }
    for (const auto& attribute : parsed.value().attributes) {
        if (attribute.type == ntfs_core::kAttrData && attribute.name.empty() &&
            attribute.non_resident) {
            return base::Result<ntfs_core::AttributeValue>::success(attribute);
        }
    }
    return detail::shrink_fail<ntfs_core::AttributeValue>(
        base::ErrorCode::kCorruptData, "ntfs_resize.audit_missing_data");
}

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
unnamed_attribute_of(detail::MftRecordStore& store, const std::uint64_t record_number,
                     const std::uint32_t attribute_type,
                     const base::CancellationToken cancellation) {
    auto raw = store.read_record_bytes(record_number, cancellation);
    if (!raw) {
        return base::Result<ntfs_core::AttributeValue>::failure(raw.error());
    }
    auto parsed = ntfs_core::parse_mft_record_bytes(raw.value(), store.geometry().bytes_per_sector,
                                                    record_number);
    if (!parsed) {
        return base::Result<ntfs_core::AttributeValue>::failure(parsed.error());
    }
    for (const auto& attribute : parsed.value().attributes) {
        if (attribute.type == attribute_type && attribute.name.empty()) {
            return base::Result<ntfs_core::AttributeValue>::success(attribute);
        }
    }
    return detail::shrink_fail<ntfs_core::AttributeValue>(base::ErrorCode::kCorruptData,
                                                           "ntfs_resize.audit_missing_data");
}

[[nodiscard]] base::Result<ntfs_core::AttributeValue>
unnamed_data_of(detail::MftRecordStore& store, const std::uint64_t record_number,
                const base::CancellationToken cancellation) {
    return unnamed_attribute_of(store, record_number, ntfs_core::kAttrData, cancellation);
}

// $MFT's unnamed $BITMAP attribute marks which records are allocated. Records with a clear bit
// are free; NTFS leaves them unformatted (often all-zero), so their content is not audited.
[[nodiscard]] std::vector<std::byte>
load_mft_record_allocation_bitmap(detail::MftRecordStore& store,
                                  ports::IRandomAccessReader& reader,
                                  const base::CancellationToken cancellation) {
    auto bitmap_attr = unnamed_attribute_of(store, detail::kFileNumberMft, ntfs_core::kAttrBitmap,
                                            cancellation);
    if (!bitmap_attr) {
        return {};
    }
    detail::NtfsVolumeView view;
    view.reader = &reader;
    view.geometry = store.geometry();
    auto payload = detail::read_attribute_payload(view, bitmap_attr.value(),
                                                  detail::kMaxBitmapLoadBytes, cancellation);
    if (!payload) {
        return {};
    }
    return std::move(payload).value();
}

[[nodiscard]] bool record_is_allocated(const std::vector<std::byte>& record_bitmap,
                                       const std::uint64_t record_number) {
    if (record_bitmap.empty()) {
        return true;
    }
    auto bit = ntfs_core::bitmap_bit_is_set(record_bitmap, record_number);
    return !bit || bit.value();
}

[[nodiscard]] base::Result<void>
check_runs_within_boundary(const std::vector<ntfs_core::DataRun>& runs,
                           const std::uint64_t new_total_cluster_count,
                           PrecommitAuditReport& report, const std::string& context) {
    for (const auto& run : runs) {
        if (run.sparse || run.cluster_count.value == 0) {
            continue;
        }
        std::uint64_t end = 0;
        if (!ntfs_core::checked_add_u64(run.first_lcn.value, run.cluster_count.value, end)) {
            add_failure(report, "ntfs_resize.audit_run_overflow",
                        context + " lcn=" + std::to_string(run.first_lcn.value) +
                            " clusters=" + std::to_string(run.cluster_count.value));
            return base::Result<void>::success();
        }
        if (end > new_total_cluster_count) {
            add_failure(report, "ntfs_resize.audit_lcn_beyond_boundary",
                        context + " run_end=" + std::to_string(end) +
                            " new_total=" + std::to_string(new_total_cluster_count));
            return base::Result<void>::success();
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
audit_all_reachable_runs(detail::MftRecordStore& store, ports::IRandomAccessReader& reader,
                         const ShrinkPlan& plan, PrecommitAuditReport& report,
                         const base::CancellationToken cancellation) {
    auto mft_data = unnamed_data_of(store, detail::kFileNumberMft, cancellation);
    if (!mft_data) {
        add_failure(report, "ntfs_resize.audit_mft_unreadable",
                    "error=" + mft_data.error().message);
        return base::Result<void>::success();
    }
    if (mft_data.value().data_size.value < store.geometry().bytes_per_mft_record) {
        add_failure(report, "ntfs_resize.audit_mft_truncated",
                    "data_size=" + std::to_string(mft_data.value().data_size.value) +
                        " record_bytes=" +
                        std::to_string(store.geometry().bytes_per_mft_record));
        return base::Result<void>::success();
    }
    const auto record_count =
        mft_data.value().data_size.value / store.geometry().bytes_per_mft_record;
    const auto new_clusters = plan.new_total_cluster_count();
    const auto record_bitmap = load_mft_record_allocation_bitmap(store, reader, cancellation);
    std::set<std::pair<std::uint64_t, std::uint64_t>> allocated_ranges;

    for (std::uint64_t record_number = 0; record_number < record_count; ++record_number) {
        if (cancellation.stop_requested()) {
            return detail::shrink_fail_void(base::ErrorCode::kCancelled, "ntfs.read_failed");
        }
        if (!record_is_allocated(record_bitmap, record_number)) {
            continue;
        }
        auto raw = store.read_record_bytes(record_number, cancellation);
        if (!raw) {
            add_failure(report, "ntfs_resize.audit_record_unreadable",
                        "record=" + std::to_string(record_number) +
                            " error=" + raw.error().message);
            continue;
        }
        auto parsed = ntfs_core::parse_mft_record_bytes(raw.value(), store.geometry().bytes_per_sector,
                                                        record_number);
        if (!parsed) {
            // Without an allocation bitmap a zeroed record cannot be told apart from a free,
            // never-formatted one; only formatted-but-unparsable records count as corruption.
            if (record_bitmap.empty() && record_bytes_zeroed(raw.value())) {
                continue;
            }
            add_failure(report, "ntfs_resize.audit_record_corrupt",
                        "record=" + std::to_string(record_number) + "/" +
                            std::to_string(record_count) + " error=" + parsed.error().message +
                            describe_record_bytes(raw.value()));
            continue;
        }
        if (!parsed.value().in_use) {
            continue;
        }
        const std::string record_context = "record=" + std::to_string(record_number);
        for (const auto& attribute : parsed.value().attributes) {
            const std::string attribute_context =
                record_context + " attr_type=" + std::to_string(attribute.type);
            if (attribute.type == ntfs_core::kAttrAttributeList) {
                if (attribute.non_resident) {
                    // Attribute List body already expanded into extension records; still validate
                    // resident forms when present.
                } else {
                    auto list = ntfs_core::parse_attribute_list(attribute.resident_data);
                    if (!list) {
                        add_failure(report, "ntfs_resize.audit_attribute_list_corrupt",
                                    record_context + " error=" + list.error().message);
                        continue;
                    }
                    auto list_ok = ntfs_core::validate_attribute_list_entries(
                        list.value(), parsed.value().record_number);
                    if (!list_ok) {
                        add_failure(report, "ntfs_resize.audit_attribute_list_invalid",
                                    record_context + " error=" + list_ok.error().message);
                    }
                }
            }
            if (!attribute.non_resident) {
                continue;
            }
            auto runs_ok = ntfs_core::validate_data_runs(attribute.runs);
            if (!runs_ok) {
                add_failure(report, "ntfs_resize.audit_runlist_invalid",
                            attribute_context + " error=" + runs_ok.error().message);
                continue;
            }
            auto boundary = check_runs_within_boundary(attribute.runs, new_clusters, report,
                                                       attribute_context);
            if (!boundary) {
                return boundary;
            }
            for (const auto& run : attribute.runs) {
                if (run.sparse || run.cluster_count.value == 0) {
                    continue;
                }
                const auto end = run.first_lcn.value + run.cluster_count.value;
                for (const auto& prior : allocated_ranges) {
                    if (run.first_lcn.value < prior.second && prior.first < end) {
                        // Same cluster owned by multiple attributes can be legal for some metadata;
                        // only flag identical full-range duplicates across different records.
                        if (prior.first == run.first_lcn.value && prior.second == end) {
                            add_failure(report, "ntfs_resize.audit_duplicate_cluster_range",
                                        attribute_context +
                                            " lcn=" + std::to_string(run.first_lcn.value) +
                                            " end=" + std::to_string(end));
                        }
                    }
                }
                allocated_ranges.insert({run.first_lcn.value, end});
            }
        }
        if (!report.failure_codes.empty() && report.failure_codes.size() > 32) {
            break;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
audit_bitmap_compatibility(detail::MftRecordStore& store, ports::IRandomAccessReader& reader,
                           const ShrinkPlan& plan, PrecommitAuditReport& report,
                           const base::CancellationToken cancellation) {
    auto bitmap_attr = unnamed_data_of(store, detail::kFileNumberBitmap, cancellation);
    if (!bitmap_attr) {
        add_failure(report, "ntfs_resize.audit_bitmap_unreadable",
                    "step=read_bitmap_record error=" + bitmap_attr.error().message);
        return base::Result<void>::success();
    }
    detail::NtfsVolumeView view;
    view.reader = &reader;
    view.geometry = store.geometry();
    auto bytes =
        detail::read_attribute_payload(view, bitmap_attr.value(), detail::kMaxBitmapLoadBytes,
                                       cancellation);
    if (!bytes) {
        add_failure(report, "ntfs_resize.audit_bitmap_unreadable",
                    "step=read_bitmap_payload error=" + bytes.error().message);
        return base::Result<void>::success();
    }
    auto covers = ntfs_core::validate_bitmap_covers_bits(plan.new_total_cluster_count(),
                                                         bytes.value().size());
    if (!covers) {
        // Bitmap may still cover old total; require at least new boundary bits exist.
        const auto required_bytes = (plan.new_total_cluster_count() + 7U) / 8U;
        if (bytes.value().size() < required_bytes) {
            add_failure(report, "ntfs_resize.audit_bitmap_too_small",
                        "bitmap_bytes=" + std::to_string(bytes.value().size()) +
                            " required_bytes=" + std::to_string(required_bytes));
        }
    }
    // Spot-check: every relocation target bit must be set; every ordinary/critical source bit clear
    // is best-effort after commits — verify targets allocated.
    for (const auto& reloc : plan.relocation_records()) {
        for (auto lcn = reloc.target.begin_lcn; lcn < reloc.target.end_lcn; ++lcn) {
            auto bit = ntfs_core::bitmap_bit_is_set(bytes.value(), lcn);
            if (!bit) {
                add_failure(report, "ntfs_resize.audit_bitmap_corrupt",
                            "lcn=" + std::to_string(lcn) +
                                " bitmap_bytes=" + std::to_string(bytes.value().size()));
                return base::Result<void>::success();
            }
            if (!bit.value()) {
                add_failure(report, "ntfs_resize.audit_bitmap_target_free",
                            "lcn=" + std::to_string(lcn));
                return base::Result<void>::success();
            }
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
audit_mft_mirror(detail::MftRecordStore& store, ports::IRandomAccessReader& reader,
                 PrecommitAuditReport& report, const base::CancellationToken cancellation) {
    auto mirr_attr = unnamed_data_of(store, 1, cancellation);
    if (!mirr_attr) {
        add_failure(report, "ntfs_resize.audit_mftmirr_unreadable",
                    "step=read_mirror_record error=" + mirr_attr.error().message);
        return base::Result<void>::success();
    }
    detail::NtfsVolumeView view;
    view.reader = &reader;
    view.geometry = store.geometry();
    const auto mirror_bytes =
        static_cast<std::uint64_t>(store.geometry().bytes_per_mft_record) * 4U;
    auto mirror_image =
        detail::read_attribute_payload(view, mirr_attr.value(), mirror_bytes, cancellation);
    if (!mirror_image) {
        add_failure(report, "ntfs_resize.audit_mftmirr_unreadable",
                    "step=read_mirror_payload error=" + mirror_image.error().message);
        return base::Result<void>::success();
    }
    if (mirror_image.value().size() < store.geometry().bytes_per_mft_record) {
        add_failure(report, "ntfs_resize.audit_mftmirr_truncated",
                    "mirror_bytes=" + std::to_string(mirror_image.value().size()) +
                        " record_bytes=" +
                        std::to_string(store.geometry().bytes_per_mft_record));
        return base::Result<void>::success();
    }
    auto primary0 = store.read_record_bytes(0, cancellation);
    if (!primary0) {
        add_failure(report, "ntfs_resize.audit_mft_unreadable",
                    "step=read_primary_record0 error=" + primary0.error().message);
        return base::Result<void>::success();
    }
    if (primary0.value().size() > mirror_image.value().size() ||
        !std::equal(primary0.value().begin(), primary0.value().end(),
                    mirror_image.value().begin())) {
        add_failure(report, "ntfs_resize.audit_mftmirr_mismatch");
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
audit_geometry_and_boot_escrow(const ShrinkPlan& plan, CompositeNtfsBlockDevice& device,
                               PrecommitAuditReport& report,
                               const base::CancellationToken cancellation) {
    if (plan.target_capacity_bytes() != device.target_capacity_bytes()) {
        add_failure(report, "ntfs_resize.audit_target_capacity_mismatch",
                    "plan=" + std::to_string(plan.target_capacity_bytes()) +
                        " device=" + std::to_string(device.target_capacity_bytes()));
    }
    if (plan.target_device_geometry().capacity_bytes != 0 &&
        plan.target_device_geometry().capacity_bytes != plan.target_capacity_bytes()) {
        add_failure(report, "ntfs_resize.audit_geometry_inconsistent",
                    "geometry=" + std::to_string(plan.target_device_geometry().capacity_bytes) +
                        " plan=" + std::to_string(plan.target_capacity_bytes()));
    }
    const auto& boot = plan.source_ntfs_geometry();
    std::uint64_t minimum_device_bytes = 0;
    const auto target_sector_aligned =
        boot.bytes_per_sector != 0 &&
        plan.target_capacity_bytes() % boot.bytes_per_sector == 0;
    const auto target_sector_count = target_sector_aligned
                                         ? plan.target_capacity_bytes() / boot.bytes_per_sector
                                         : 0;
    if (!target_sector_aligned || boot.sectors_per_cluster == 0 ||
        target_sector_count <= 1U ||
        plan.new_total_sector_count() != target_sector_count - 1U ||
        plan.new_total_cluster_count() !=
            plan.new_total_sector_count() / boot.sectors_per_cluster ||
        !ntfs_core::checked_mul_u64(plan.new_total_cluster_count(), boot.bytes_per_cluster,
                                    minimum_device_bytes) ||
        !ntfs_core::checked_add_u64(minimum_device_bytes, boot.bytes_per_sector,
                                    minimum_device_bytes) ||
        minimum_device_bytes > plan.target_capacity_bytes()) {
        add_failure(report, "ntfs_resize.audit_new_size_invalid",
                    "target_capacity=" + std::to_string(plan.target_capacity_bytes()) +
                        " bytes_per_sector=" + std::to_string(boot.bytes_per_sector) +
                        " sectors_per_cluster=" + std::to_string(boot.sectors_per_cluster) +
                        " new_sectors=" + std::to_string(plan.new_total_sector_count()) +
                        " new_clusters=" + std::to_string(plan.new_total_cluster_count()));
    }
    // Protected Boot escrow must still be readable from composite (source path).
    for (const auto& range : plan.protected_ranges()) {
        if (range.end <= range.begin || range.end > device.source_logical_size_bytes()) {
            add_failure(report, "ntfs_resize.audit_protected_range_invalid",
                        "begin=" + std::to_string(range.begin) +
                            " end=" + std::to_string(range.end) + " source_size=" +
                            std::to_string(device.source_logical_size_bytes()));
            continue;
        }
        const auto size = static_cast<std::size_t>(range.end - range.begin);
        std::vector<std::byte> buffer(size);
        auto read = device.read_at(range.begin, buffer, cancellation);
        if (!read || read.value() != size) {
            add_failure(report, "ntfs_resize.audit_boot_escrow_unreadable",
                        "begin=" + std::to_string(range.begin) +
                            " end=" + std::to_string(range.end) +
                            (read ? std::string{} : " error=" + read.error().message));
        }
    }
    return base::Result<void>::success();
}

} // namespace

base::Result<PrecommitAuditReport>
NtfsPrecommitAuditor::audit(const PrecommitAuditRequest& request,
                            const base::CancellationToken cancellation) {
    if (auto valid = validate_request(request); !valid) {
        return base::Result<PrecommitAuditReport>::failure(valid.error());
    }
    PrecommitAuditReport report;
    report.passed = true;

    TargetReaderAdapter target_reader(*request.target);
    auto geometry = candidate_geometry(*request.plan);
    auto mft_data = geometry ? load_target_mft_data(target_reader, geometry.value(), cancellation)
                             : base::Result<ntfs_core::AttributeValue>::failure(geometry.error());
    if (!geometry || !mft_data) {
        add_failure(report, "ntfs_resize.audit_volume_unreadable",
                    "error=" + (!geometry ? geometry.error().message : mft_data.error().message));
        return base::Result<PrecommitAuditReport>::success(std::move(report));
    }
    detail::MftRecordStore store(target_reader, geometry.value(), std::move(mft_data).value());

    if (auto status =
            audit_geometry_and_boot_escrow(*request.plan, *request.device, report, cancellation);
        !status) {
        return base::Result<PrecommitAuditReport>::failure(status.error());
    }
    if (auto status =
            audit_all_reachable_runs(store, target_reader, *request.plan, report, cancellation);
        !status) {
        return base::Result<PrecommitAuditReport>::failure(status.error());
    }
    if (auto status =
            audit_bitmap_compatibility(store, target_reader, *request.plan, report, cancellation);
        !status) {
        return base::Result<PrecommitAuditReport>::failure(status.error());
    }
    if (auto status = audit_mft_mirror(store, target_reader, report, cancellation); !status) {
        return base::Result<PrecommitAuditReport>::failure(status.error());
    }

    if (!report.failure_codes.empty()) {
        report.passed = false;
    }
    return base::Result<PrecommitAuditReport>::success(std::move(report));
}

} // namespace aegra::ntfs_resize
