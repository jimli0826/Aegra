#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/base/error.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] const format::Volume* find_volume(const format::Manifest& manifest,
                                                const std::uint32_t volume_index) {
    const auto volume = std::find_if(manifest.volumes.begin(), manifest.volumes.end(),
                                     [volume_index](const format::Volume& candidate) {
                                         return candidate.volume_index == volume_index;
                                     });
    return volume == manifest.volumes.end() ? nullptr : &*volume;
}

} // namespace

struct PersonalArchiveVolumeReader::Impl final {
    ports::IRecoveryPointReader* inner{nullptr};
    std::uint64_t logical_size{0};
    std::vector<std::uint64_t> inner_chunk_indices;
};

PersonalArchiveVolumeReader::PersonalArchiveVolumeReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalArchiveVolumeReader::~PersonalArchiveVolumeReader() = default;

base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>
PersonalArchiveVolumeReader::open(ports::IRecoveryPointReader& inner,
                                  const format::Manifest& manifest,
                                  const std::uint32_t volume_index) {
    const auto* volume = find_volume(manifest, volume_index);
    if (volume == nullptr || volume->total_size == 0) {
        return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::failure(
            error(base::ErrorCode::kInvalidArgument, "volume is missing from archive manifest"));
    }
    auto implementation = std::make_unique<Impl>();
    implementation->inner = &inner;
    implementation->logical_size = volume->total_size;
    implementation->inner_chunk_indices.reserve(static_cast<std::size_t>(inner.chunk_count()));
    std::uint64_t expected_offset = 0;
    for (std::uint64_t index = 0; index < inner.chunk_count(); ++index) {
        auto descriptor = inner.describe_chunk(index);
        if (!descriptor) {
            return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::failure(
                descriptor.error());
        }
        if (descriptor.value().source_index != volume_index) {
            continue;
        }
        if (descriptor.value().logical_offset != expected_offset) {
            return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::failure(error(
                base::ErrorCode::kCorruptData, "volume chunks are not contiguous in archive"));
        }
        if (descriptor.value().logical_size == 0 ||
            expected_offset > volume->total_size ||
            descriptor.value().logical_size > volume->total_size - expected_offset) {
            return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::failure(
                error(base::ErrorCode::kCorruptData, "volume chunk exceeds volume size"));
        }
        expected_offset += descriptor.value().logical_size;
        implementation->inner_chunk_indices.push_back(index);
    }
    if (expected_offset != volume->total_size) {
        return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::failure(
            error(base::ErrorCode::kCorruptData, "volume chunks do not cover volume size"));
    }
    return base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>::success(
        std::unique_ptr<PersonalArchiveVolumeReader>(
            new PersonalArchiveVolumeReader(std::move(implementation))));
}

std::uint64_t PersonalArchiveVolumeReader::logical_size_bytes() const noexcept {
    return implementation_->logical_size;
}

std::uint64_t PersonalArchiveVolumeReader::chunk_count() const noexcept {
    return implementation_->inner_chunk_indices.size();
}

base::Result<ports::ChunkDescriptor>
PersonalArchiveVolumeReader::describe_chunk(const std::uint64_t chunk_index) const {
    if (chunk_index >= implementation_->inner_chunk_indices.size()) {
        return base::Result<ports::ChunkDescriptor>::failure(
            error(base::ErrorCode::kNotFound, "volume chunk does not exist"));
    }
    auto descriptor = implementation_->inner->describe_chunk(
        implementation_->inner_chunk_indices[static_cast<std::size_t>(chunk_index)]);
    if (!descriptor) {
        return descriptor;
    }
    descriptor.value().source_index = 0;
    descriptor.value().chunk_index = chunk_index;
    return descriptor;
}

base::Result<ports::ChunkData>
PersonalArchiveVolumeReader::read_chunk(const std::uint64_t chunk_index,
                                        const base::CancellationToken cancellation) {
    if (chunk_index >= implementation_->inner_chunk_indices.size()) {
        return base::Result<ports::ChunkData>::failure(
            error(base::ErrorCode::kNotFound, "volume chunk does not exist"));
    }
    auto chunk = implementation_->inner->read_chunk(
        implementation_->inner_chunk_indices[static_cast<std::size_t>(chunk_index)], cancellation);
    if (!chunk) {
        return chunk;
    }
    chunk.value().descriptor.source_index = 0;
    chunk.value().descriptor.chunk_index = chunk_index;
    return chunk;
}

} // namespace aegra::adapters::personal_archive
