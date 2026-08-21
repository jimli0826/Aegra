#pragma once

#include "aegra/base/result.h"
#include "aegra/ntfs_core/types.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstdint>
#include <string>
#include <vector>

namespace aegra::ntfs_resize {

inline constexpr std::uint32_t kShrinkPlanVersion = 1;

/// Half-open byte range [begin, end).
struct ByteRange final {
    std::uint64_t begin{0};
    std::uint64_t end{0};

    [[nodiscard]] bool operator==(const ByteRange&) const noexcept = default;
};

/// Half-open logical cluster range [begin_lcn, end_lcn).
struct ClusterRange final {
    std::uint64_t begin_lcn{0};
    std::uint64_t end_lcn{0};

    [[nodiscard]] bool operator==(const ClusterRange&) const noexcept = default;
};

struct RelocationRecord final {
    ClusterRange source{};
    ClusterRange target{};
    std::uint64_t cluster_count{0};
    std::uint64_t mft_record_number{0};
    std::uint32_t attribute_type{0};
    std::u16string attribute_name; // empty = unnamed
    std::uint16_t attribute_id{0};
    std::uint32_t plan_order{0};

    [[nodiscard]] bool operator==(const RelocationRecord&) const noexcept = default;
};

enum class MetadataMutationKind : std::uint32_t {
    kRunlistReplace = 1,
    kAttributeResize = 2,
    kRecordRewrite = 3,
    kAttributeListUpdate = 4,
};

struct MetadataMutation final {
    std::uint64_t mft_record_number{0};
    std::uint16_t expected_record_sequence{0};
    std::uint32_t attribute_type{0};
    std::u16string attribute_name;
    std::uint16_t attribute_id{0};
    std::uint32_t attribute_record_offset{0};
    std::uint32_t attribute_length{0};
    std::uint16_t runlist_offset{0};
    MetadataMutationKind kind{MetadataMutationKind::kRunlistReplace};
    std::string preimage_digest;
    std::vector<std::byte> replacement_runlist;

    [[nodiscard]] bool operator==(const MetadataMutation&) const noexcept = default;
};

enum class CriticalFileOperationKind : std::uint32_t {
    kRelocateData = 1,
    kInvalidateLogfile = 2,
    kUpdateVolumeSize = 3,
    kUpdateBitmap = 4,
    kRelocateMft = 5,
    kUpdateBootGeometry = 6,
};

struct CriticalFileOperation final {
    std::uint32_t file_number{0}; // 0=$MFT, 1=$MFTMirr, ...
    CriticalFileOperationKind operation_kind{CriticalFileOperationKind::kRelocateData};
    std::uint32_t order{0};

    [[nodiscard]] bool operator==(const CriticalFileOperation&) const noexcept = default;
};

class ShrinkPlanBuilder;

/// Immutable analysis→execution contract (ADR-0025). Generated once, then read-only.
class ShrinkPlan final {
  public:
    [[nodiscard]] std::uint32_t plan_version() const noexcept {
        return plan_version_;
    }
    [[nodiscard]] const std::string& source_chain_fingerprint() const noexcept {
        return source_chain_fingerprint_;
    }
    [[nodiscard]] std::uint32_t source_volume_index() const noexcept {
        return source_volume_index_;
    }
    [[nodiscard]] std::uint64_t source_logical_size_bytes() const noexcept {
        return source_logical_size_bytes_;
    }
    [[nodiscard]] const std::string& source_boot_digest() const noexcept {
        return source_boot_digest_;
    }
    [[nodiscard]] const ntfs_core::BootGeometry& source_ntfs_geometry() const noexcept {
        return source_ntfs_geometry_;
    }
    [[nodiscard]] const std::string& target_stable_id_digest() const noexcept {
        return target_stable_id_digest_;
    }
    [[nodiscard]] const ports::BlockDeviceGeometry& target_device_geometry() const noexcept {
        return target_device_geometry_;
    }
    [[nodiscard]] std::uint64_t target_capacity_bytes() const noexcept {
        return target_capacity_bytes_;
    }
    [[nodiscard]] std::uint64_t new_total_sector_count() const noexcept {
        return new_total_sector_count_;
    }
    [[nodiscard]] std::uint64_t new_total_cluster_count() const noexcept {
        return new_total_cluster_count_;
    }
    [[nodiscard]] std::uint64_t new_mft_start_lcn() const noexcept {
        return new_mft_start_lcn_;
    }
    [[nodiscard]] std::uint64_t new_mft_mirror_start_lcn() const noexcept {
        return new_mft_mirror_start_lcn_;
    }
    [[nodiscard]] std::uint64_t minimum_target_bytes() const noexcept {
        return minimum_target_bytes_;
    }
    [[nodiscard]] std::uint64_t scratch_upper_bound_bytes() const noexcept {
        return scratch_upper_bound_bytes_;
    }
    [[nodiscard]] const std::vector<ByteRange>& protected_ranges() const noexcept {
        return protected_ranges_;
    }
    [[nodiscard]] const std::vector<RelocationRecord>& relocation_records() const noexcept {
        return relocation_records_;
    }
    [[nodiscard]] const std::vector<MetadataMutation>& metadata_mutations() const noexcept {
        return metadata_mutations_;
    }
    [[nodiscard]] const std::vector<CriticalFileOperation>& critical_file_operations()
        const noexcept {
        return critical_file_operations_;
    }
    /// 64 lowercase hex characters (SHA-256 of canonical payload).
    [[nodiscard]] const std::string& plan_payload_digest() const noexcept {
        return plan_payload_digest_;
    }

  private:
    friend class ShrinkPlanBuilder;

    ShrinkPlan() = default;

    std::uint32_t plan_version_{kShrinkPlanVersion};
    std::string source_chain_fingerprint_;
    std::uint32_t source_volume_index_{0};
    std::uint64_t source_logical_size_bytes_{0};
    std::string source_boot_digest_;
    ntfs_core::BootGeometry source_ntfs_geometry_{};
    std::string target_stable_id_digest_;
    ports::BlockDeviceGeometry target_device_geometry_{};
    std::uint64_t target_capacity_bytes_{0};
    std::uint64_t new_total_sector_count_{0};
    std::uint64_t new_total_cluster_count_{0};
    std::uint64_t new_mft_start_lcn_{0};
    std::uint64_t new_mft_mirror_start_lcn_{0};
    std::uint64_t minimum_target_bytes_{0};
    std::uint64_t scratch_upper_bound_bytes_{0};
    std::vector<ByteRange> protected_ranges_;
    std::vector<RelocationRecord> relocation_records_;
    std::vector<MetadataMutation> metadata_mutations_;
    std::vector<CriticalFileOperation> critical_file_operations_;
    std::string plan_payload_digest_;
};

class ShrinkPlanBuilder final {
  public:
    ShrinkPlanBuilder& set_plan_version(std::uint32_t version) noexcept;
    ShrinkPlanBuilder& set_source_chain_fingerprint(std::string fingerprint);
    ShrinkPlanBuilder& set_source_volume_index(std::uint32_t index) noexcept;
    ShrinkPlanBuilder& set_source_logical_size_bytes(std::uint64_t bytes) noexcept;
    ShrinkPlanBuilder& set_source_boot_digest(std::string digest);
    ShrinkPlanBuilder& set_source_ntfs_geometry(ntfs_core::BootGeometry geometry);
    ShrinkPlanBuilder& set_target_stable_id_digest(std::string digest);
    ShrinkPlanBuilder& set_target_device_geometry(ports::BlockDeviceGeometry geometry);
    ShrinkPlanBuilder& set_target_capacity_bytes(std::uint64_t bytes) noexcept;
    ShrinkPlanBuilder& set_new_total_sector_count(std::uint64_t count) noexcept;
    ShrinkPlanBuilder& set_new_total_cluster_count(std::uint64_t count) noexcept;
    ShrinkPlanBuilder& set_new_mft_start_lcn(std::uint64_t lcn) noexcept;
    ShrinkPlanBuilder& set_new_mft_mirror_start_lcn(std::uint64_t lcn) noexcept;
    ShrinkPlanBuilder& set_minimum_target_bytes(std::uint64_t bytes) noexcept;
    ShrinkPlanBuilder& set_scratch_upper_bound_bytes(std::uint64_t bytes) noexcept;
    ShrinkPlanBuilder& set_protected_ranges(std::vector<ByteRange> ranges);
    ShrinkPlanBuilder& set_relocation_records(std::vector<RelocationRecord> records);
    ShrinkPlanBuilder& set_metadata_mutations(std::vector<MetadataMutation> mutations);
    ShrinkPlanBuilder& set_critical_file_operations(std::vector<CriticalFileOperation> operations);

    /// Validates invariants and fills plan_payload_digest from the canonical payload.
    [[nodiscard]] base::Result<ShrinkPlan> build();

  private:
    ShrinkPlan draft_{};
};

} // namespace aegra::ntfs_resize
