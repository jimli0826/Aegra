#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"
#include "aegra/ntfs_resize/shrink_plan.h"
#include "aegra/ports/random_access.h"
#include "aegra/ports/random_access_block_device.h"

#include <cstdint>
#include <string>
#include <string_view>

namespace aegra::ntfs_resize {

struct NtfsShrinkCandidateSnapshot final {
    std::uint64_t expected_source_logical_size_bytes{0};
    std::uint64_t target_capacity_bytes{0};
    ntfs_core::BootGeometry source_geometry{};
    ports::BlockDeviceGeometry target_geometry{};
};

struct NtfsShrinkCountSnapshot final {
    std::uint64_t target_capacity_bytes{0};
    std::uint64_t new_total_sector_count{0};
    std::uint64_t new_total_cluster_count{0};
};

struct NtfsShrinkAllocationSnapshot final {
    std::uint64_t target_capacity_bytes{0};
    std::uint64_t boundary_cluster_count{0};
    std::uint64_t allocated_beyond_cluster_count{0};
    std::uint64_t allocatable_cluster_count{0};
    std::uint64_t reserved_cluster_count{0};
};

struct NtfsShrinkMftRecordSnapshot final {
    std::uint64_t target_capacity_bytes{0};
    std::uint64_t record_number{0};
    std::uint64_t mft_file_offset_bytes{0};
    std::uint64_t source_device_offset_bytes{0};
    std::uint32_t record_size_bytes{0};
    std::uint32_t signature_hex{0};
    std::uint16_t update_sequence_array_offset{0};
    std::uint16_t update_sequence_array_count{0};
    std::uint16_t first_attribute_offset{0};
    std::uint16_t flags{0};
    std::uint32_t used_size_bytes{0};
    bool read_via_mft_data{false};
    bool source_device_offset_known{false};
    bool parsed{false};
    base::ErrorCode error_code{base::ErrorCode::kNone};
    std::string_view message_code;
};

/// Optional, non-owning diagnostic observer. Callbacks must not throw or affect analysis results.
class INtfsShrinkAnalysisObserver {
  public:
    INtfsShrinkAnalysisObserver() = default;
    virtual ~INtfsShrinkAnalysisObserver() = default;
    INtfsShrinkAnalysisObserver(const INtfsShrinkAnalysisObserver&) = delete;
    INtfsShrinkAnalysisObserver& operator=(const INtfsShrinkAnalysisObserver&) = delete;

    virtual void candidate_geometry(const NtfsShrinkCandidateSnapshot& snapshot) noexcept = 0;
    virtual void candidate_counts(const NtfsShrinkCountSnapshot& snapshot) noexcept = 0;
    virtual void candidate_allocation(const NtfsShrinkAllocationSnapshot& snapshot) noexcept = 0;
    virtual void candidate_stage(std::uint64_t target_capacity_bytes,
                                 std::string_view stage) noexcept = 0;
    virtual void mft_record(const NtfsShrinkMftRecordSnapshot& snapshot) noexcept = 0;
};

struct NtfsShrinkAnalyzeRequest final {
    ports::IRandomAccessReader* source_volume{nullptr};
    std::uint64_t expected_source_logical_size_bytes{0};
    std::uint32_t source_volume_index{0};
    std::string source_chain_fingerprint;
    std::uint64_t target_capacity_bytes{0};
    ports::BlockDeviceGeometry target_geometry{};
    std::string target_stable_id_digest;
    INtfsShrinkAnalysisObserver* observer{nullptr};
};

class NtfsShrinkAnalyzer final {
  public:
    NtfsShrinkAnalyzer() = delete;

    /// Read-only full NTFS analysis. Must not open a Target write handle.
    [[nodiscard]] static base::Result<ShrinkPlan> analyze(const NtfsShrinkAnalyzeRequest& request,
                                                          base::CancellationToken cancellation);
};

} // namespace aegra::ntfs_resize
