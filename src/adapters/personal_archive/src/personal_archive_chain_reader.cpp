#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_block_worker_pool.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

struct OverlaySlice final {
    std::size_t layer_index{0};
    std::uint64_t source_chunk_index{0};
    std::uint64_t source_offset{0};
    std::uint64_t target_offset{0};
    std::uint64_t size{0};
};

struct ChainRecord final {
    ports::ChunkDescriptor descriptor;
    std::vector<OverlaySlice> overlays;
};

void remove_free_range(std::vector<ports::ChunkFreeRange>& ranges, const std::uint64_t offset,
                       const std::uint64_t size) {
    const auto end = offset + size;
    std::vector<ports::ChunkFreeRange> result;
    result.reserve(ranges.size() + 1);
    for (const auto& range : ranges) {
        const auto range_end = range.offset + range.size;
        if (range_end <= offset || range.offset >= end) {
            result.push_back(range);
            continue;
        }
        if (range.offset < offset) {
            result.push_back({range.offset, offset - range.offset});
        }
        if (range_end > end) {
            result.push_back({end, range_end - end});
        }
    }
    ranges = std::move(result);
}

void add_free_range(std::vector<ports::ChunkFreeRange>& ranges, ports::ChunkFreeRange added) {
    ranges.push_back(added);
    std::ranges::sort(ranges, {}, &ports::ChunkFreeRange::offset);
    std::vector<ports::ChunkFreeRange> merged;
    merged.reserve(ranges.size());
    for (const auto& range : ranges) {
        if (!merged.empty() && merged.back().offset + merged.back().size == range.offset) {
            merged.back().size += range.size;
        } else {
            merged.push_back(range);
        }
    }
    ranges = std::move(merged);
}

void apply_free_overlay(ChainRecord& record, const ports::ChunkDescriptor& overlay,
                        const OverlaySlice& slice) {
    remove_free_range(record.descriptor.free_ranges, slice.target_offset, slice.size);
    const auto source_end = slice.source_offset + slice.size;
    for (const auto& range : overlay.free_ranges) {
        const auto range_end = range.offset + range.size;
        const auto start = (std::max)(range.offset, slice.source_offset);
        const auto end = (std::min)(range_end, source_end);
        if (start < end) {
            add_free_range(record.descriptor.free_ranges,
                           {slice.target_offset + start - slice.source_offset, end - start});
        }
    }
}

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

/// Layer geometry must match the write-side incremental parent check: same ordered
/// volume_index / volume_id / total_size for every source volume (not single-volume only).
[[nodiscard]] bool same_volume_geometry(const format::Manifest& left,
                                        const format::Manifest& right) noexcept {
    if (left.volumes.size() != right.volumes.size() || left.volumes.empty()) {
        return false;
    }
    for (std::size_t index = 0; index < left.volumes.size(); ++index) {
        const auto& left_volume = left.volumes[index];
        const auto& right_volume = right.volumes[index];
        if (left_volume.volume_index != right_volume.volume_index ||
            left_volume.volume_id != right_volume.volume_id ||
            left_volume.total_size != right_volume.total_size) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] base::Result<void> validate_layer(const PersonalArchiveReader& previous,
                                                const PersonalArchiveReader& current) {
    const auto& previous_identity = previous.identity();
    const auto& current_identity = current.identity();
    if (current_identity.backup_type != format::BackupType::kIncremental ||
        current_identity.parent_uuid != previous_identity.file_uuid ||
        current_identity.backup_set_uuid != previous_identity.backup_set_uuid) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain identity is invalid"));
    }
    if (current_identity.block_size != previous_identity.block_size ||
        !same_volume_geometry(previous.manifest(), current.manifest())) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain source geometry changed"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_chain(const std::vector<std::unique_ptr<PersonalArchiveReader>>& layers) {
    if (layers.front()->identity().backup_type != format::BackupType::kFull) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "archive chain must begin with a full backup"));
    }
    std::vector<std::array<std::byte, 16>> identities;
    identities.reserve(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto& identity = layers[index]->identity().file_uuid;
        if (std::find(identities.begin(), identities.end(), identity) != identities.end()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kConflict, "archive chain contains a repeated UUID"));
        }
        identities.push_back(identity);
        if (index != 0) {
            auto valid = validate_layer(*layers[index - 1], *layers[index]);
            if (!valid) {
                return valid;
            }
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::vector<ChainRecord>>
make_base_records(const PersonalArchiveReader& base) {
    std::vector<ChainRecord> result;
    result.reserve(static_cast<std::size_t>(base.chunk_count()));
    for (std::uint64_t index = 0; index < base.chunk_count(); ++index) {
        auto descriptor = base.describe_chunk(index);
        if (!descriptor) {
            return base::Result<std::vector<ChainRecord>>::failure(descriptor.error());
        }
        result.push_back({descriptor.value(), {}});
    }
    return base::Result<std::vector<ChainRecord>>::success(std::move(result));
}

void append_overlay(std::vector<ChainRecord>& records, const std::size_t layer_index,
                    const ports::ChunkDescriptor& overlay) {
    // logical_offset is per source volume (restarts at 0); match source_index first.
    const auto overlay_end = overlay.logical_offset + overlay.logical_size;
    for (auto& record : records) {
        if (record.descriptor.source_index != overlay.source_index) {
            continue;
        }
        const auto base_start = record.descriptor.logical_offset;
        const auto base_end = base_start + record.descriptor.logical_size;
        if (base_end <= overlay.logical_offset) {
            continue;
        }
        if (base_start >= overlay_end) {
            break;
        }
        const auto start = (std::max)(base_start, overlay.logical_offset);
        const auto end = (std::min)(base_end, overlay_end);
        OverlaySlice slice{layer_index, overlay.chunk_index, start - overlay.logical_offset,
                           start - base_start, end - start};
        record.overlays.push_back(slice);
        apply_free_overlay(record, overlay, slice);
    }
}

[[nodiscard]] base::Result<void> add_layer_overlays(std::vector<ChainRecord>& records,
                                                    const std::size_t layer_index,
                                                    const PersonalArchiveReader& layer) {
    for (std::uint64_t index = 0; index < layer.chunk_count(); ++index) {
        auto descriptor = layer.describe_chunk(index);
        if (!descriptor) {
            return base::Result<void>::failure(descriptor.error());
        }
        append_overlay(records, layer_index, descriptor.value());
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
apply_overlay(const OverlaySlice& overlay,
              std::vector<std::unique_ptr<PersonalArchiveReader>>& layers,
              std::vector<std::byte>& target, const base::CancellationToken& cancellation) {
    auto source = layers[overlay.layer_index]->read_chunk(overlay.source_chunk_index, cancellation);
    if (!source) {
        return base::Result<void>::failure(source.error());
    }
    if (overlay.source_offset > source.value().payload.size() ||
        overlay.size > source.value().payload.size() - overlay.source_offset ||
        overlay.target_offset > target.size() ||
        overlay.size > target.size() - overlay.target_offset) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "archive chain overlay is out of range"));
    }
    const auto source_offset = static_cast<std::size_t>(overlay.source_offset);
    const auto target_offset = static_cast<std::size_t>(overlay.target_offset);
    const auto size = static_cast<std::size_t>(overlay.size);
    std::copy_n(source.value().payload.begin() + static_cast<std::ptrdiff_t>(source_offset), size,
                target.begin() + static_cast<std::ptrdiff_t>(target_offset));
    return base::Result<void>::success();
}

} // namespace

base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>
PersonalArchiveChainReader::open_layers(
    const ArchiveChainOpenRequest& request,
    const std::shared_ptr<detail::BlockWorkerPool>& block_workers) {
    if (request.layers.empty() || request.maximum_chain_depth == 0 ||
        request.layers.size() > request.maximum_chain_depth) {
        return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::failure(
            error(base::ErrorCode::kInvalidArgument, "archive chain request is invalid"));
    }
    std::vector<std::unique_ptr<PersonalArchiveReader>> result;
    result.reserve(request.layers.size());
    for (const auto& layer_request : request.layers) {
        auto layer = PersonalArchiveReader::open_with_workers(layer_request, block_workers);
        if (!layer) {
            return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::failure(
                layer.error());
        }
        result.push_back(std::move(layer).value());
    }
    return base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>::success(
        std::move(result));
}

struct PersonalArchiveChainReader::Impl final {
    format::Manifest manifest;
    std::vector<std::unique_ptr<PersonalArchiveReader>> layers;
    std::vector<ChainRecord> records;
};

PersonalArchiveChainReader::PersonalArchiveChainReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveChainReader::~PersonalArchiveChainReader() = default;

base::Result<std::unique_ptr<PersonalArchiveChainReader>>
PersonalArchiveChainReader::open(const ArchiveChainOpenRequest& request) {
    auto block_workers =
        std::make_shared<detail::BlockWorkerPool>(detail::default_block_worker_count());
    auto layers = open_layers(request, block_workers);
    if (!layers) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(layers.error());
    }
    auto valid = validate_chain(layers.value());
    if (!valid) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(valid.error());
    }
    auto records = make_base_records(*layers.value().front());
    if (!records) {
        return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(records.error());
    }
    for (std::size_t index = 1; index < layers.value().size(); ++index) {
        auto added = add_layer_overlays(records.value(), index, *layers.value()[index]);
        if (!added) {
            return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::failure(
                added.error());
        }
    }
    auto implementation = std::make_unique<Impl>();
    implementation->manifest = layers.value().back()->manifest();
    implementation->layers = std::move(layers).value();
    implementation->records = std::move(records).value();
    return base::Result<std::unique_ptr<PersonalArchiveChainReader>>::success(
        std::unique_ptr<PersonalArchiveChainReader>(
            new PersonalArchiveChainReader(std::move(implementation))));
}

const format::Manifest& PersonalArchiveChainReader::manifest() const noexcept {
    return implementation_->manifest;
}

std::uint64_t PersonalArchiveChainReader::logical_size_bytes() const noexcept {
    return implementation_->layers.front()->logical_size_bytes();
}

std::uint64_t PersonalArchiveChainReader::chunk_count() const noexcept {
    return implementation_->records.size();
}

base::Result<ports::ChunkDescriptor>
PersonalArchiveChainReader::describe_chunk(const std::uint64_t chunk_index) const {
    if (chunk_index >= implementation_->records.size()) {
        return base::Result<ports::ChunkDescriptor>::failure(
            error(base::ErrorCode::kNotFound, "archive chain chunk does not exist"));
    }
    return base::Result<ports::ChunkDescriptor>::success(
        implementation_->records[static_cast<std::size_t>(chunk_index)].descriptor);
}

base::Result<ports::ChunkData>
PersonalArchiveChainReader::read_chunk(const std::uint64_t chunk_index,
                                       const base::CancellationToken cancellation) {
    auto descriptor = describe_chunk(chunk_index);
    if (!descriptor) {
        return base::Result<ports::ChunkData>::failure(descriptor.error());
    }
    auto base = implementation_->layers.front()->read_chunk(chunk_index, cancellation);
    if (!base) {
        return base::Result<ports::ChunkData>::failure(base.error());
    }
    const auto& record = implementation_->records[static_cast<std::size_t>(chunk_index)];
    for (const auto& overlay : record.overlays) {
        auto applied =
            apply_overlay(overlay, implementation_->layers, base.value().payload, cancellation);
        if (!applied) {
            return base::Result<ports::ChunkData>::failure(applied.error());
        }
    }
    return base::Result<ports::ChunkData>::success(
        {descriptor.value(), std::move(base).value().payload});
}

} // namespace aegra::adapters::personal_archive
