#include "aegra/adapters/ntfs/ntfs_reader.h"

#include "ntfs_internal.h"

#include <algorithm>
#include <cstring>
#include <utility>
#include <vector>

namespace aegra::adapters::ntfs {
namespace {

using detail::AttributeValue;
using detail::build_file_layout;
using detail::checked_add_u64;
using detail::checked_mul_u64;
using detail::decode_continuation;
using detail::encode_continuation;
using detail::FileDataLayout;
using detail::LruCache;
using detail::make_error;
using detail::parse_attribute_list;
using detail::parse_boot_sector;
using detail::parse_index_entries;
using detail::parse_mft_record_bytes;
using detail::ParsedMftRecord;
using detail::read_from_attribute;
using detail::read_u16;
using detail::read_u32;
using detail::to_boot_geometry;
using detail::to_volume_info;

[[nodiscard]] base::Result<std::vector<std::byte>>
load_index_bitmap(ports::IRandomAccessReader& reader, const NtfsVolumeInfo& info,
                  const AttributeValue& bitmap_attr, const std::uint64_t buffer_count,
                  const base::CancellationToken cancellation) {
    if (bitmap_attr.compressed) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.compressed_unsupported"));
    }
    auto bitmap_ok =
        ntfs_core::validate_bitmap_covers_bits(buffer_count, bitmap_attr.data_size.value);
    if (!bitmap_ok) {
        return base::Result<std::vector<std::byte>>::failure(bitmap_ok.error());
    }
    constexpr std::uint64_t kMaxIndexBitmapBytes = 16ULL * 1024ULL * 1024ULL;
    if (bitmap_attr.data_size.value > kMaxIndexBitmapBytes) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(bitmap_attr.data_size.value),
                                 std::byte{0});
    auto bitmap_read =
        read_from_attribute(reader, to_boot_geometry(info), bitmap_attr, ntfs_core::ByteOffset{0},
                            std::span<std::byte>(bytes), cancellation);
    if (!bitmap_read) {
        return base::Result<std::vector<std::byte>>::failure(bitmap_read.error());
    }
    if (bitmap_read.value() != bytes.size()) {
        return base::Result<std::vector<std::byte>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    return base::Result<std::vector<std::byte>>::success(std::move(bytes));
}

[[nodiscard]] base::Result<std::vector<NtfsEntry>>
parse_indx_buffer(std::span<std::byte> record, const std::uint32_t bytes_per_sector) {
    if (std::memcmp(record.data(), "INDX", 4) != 0) {
        return base::Result<std::vector<NtfsEntry>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    auto fix = detail::apply_fixup(record, bytes_per_sector,
                                   read_u16(std::span<const std::byte>(record), 4),
                                   read_u16(std::span<const std::byte>(record), 6));
    if (!fix) {
        return base::Result<std::vector<NtfsEntry>>::failure(fix.error());
    }
    constexpr std::size_t kIndexHeaderOffset = 0x18;
    if (record.size() < kIndexHeaderOffset + 16) {
        return base::Result<std::vector<NtfsEntry>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    const auto first_entry = read_u32(std::span<const std::byte>(record), kIndexHeaderOffset);
    const auto total_size = read_u32(std::span<const std::byte>(record), kIndexHeaderOffset + 4);
    const auto entries_base = kIndexHeaderOffset + first_entry;
    if (entries_base > record.size() || kIndexHeaderOffset + total_size > record.size() ||
        first_entry > total_size) {
        return base::Result<std::vector<NtfsEntry>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
    }
    return parse_index_entries(
        std::span<const std::byte>(record).subspan(entries_base, total_size - first_entry));
}

} // namespace

struct NtfsVolumeReader::Impl final {
    ports::IRandomAccessReader* reader{nullptr};
    NtfsVolumeInfo info{};
    AttributeValue mft_data{};
    bool mft_ready{false};
    LruCache<std::uint64_t, FileDataLayout> layout_cache{kMftRecordCacheCapacity};
    LruCache<std::uint64_t, std::vector<std::byte>> index_cache{kIndexPageCacheCapacity};

    [[nodiscard]] base::Result<std::vector<std::byte>>
    read_bytes(const std::uint64_t offset, const std::size_t size,
               const base::CancellationToken cancellation) const {
        std::vector<std::byte> buffer(size);
        std::size_t total = 0;
        while (total < size) {
            if (cancellation.stop_requested()) {
                return base::Result<std::vector<std::byte>>::failure(
                    make_error(base::ErrorCode::kCancelled, "ntfs.read_failed"));
            }
            auto n = reader->read_at(offset + total,
                                     std::span<std::byte>(buffer.data() + total, size - total),
                                     cancellation);
            if (!n) {
                return base::Result<std::vector<std::byte>>::failure(n.error());
            }
            if (n.value() == 0) {
                return base::Result<std::vector<std::byte>>::failure(
                    make_error(base::ErrorCode::kIoFailure, "ntfs.read_failed"));
            }
            total += n.value();
        }
        return base::Result<std::vector<std::byte>>::success(std::move(buffer));
    }

    [[nodiscard]] base::Result<ParsedMftRecord>
    read_mft_record(const std::uint64_t record_number, const base::CancellationToken cancellation) {
        std::vector<std::byte> record(info.bytes_per_mft_record);
        if (!mft_ready || record_number == 0) {
            std::uint64_t offset = 0;
            if (!checked_mul_u64(info.mft_start_cluster, info.bytes_per_cluster, offset) ||
                !checked_add_u64(
                    offset, record_number * static_cast<std::uint64_t>(info.bytes_per_mft_record),
                    offset)) {
                return base::Result<ParsedMftRecord>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
            }
            auto bytes = read_bytes(offset, info.bytes_per_mft_record, cancellation);
            if (!bytes) {
                return base::Result<ParsedMftRecord>::failure(bytes.error());
            }
            record = std::move(bytes).value();
        } else {
            const auto file_offset =
                record_number * static_cast<std::uint64_t>(info.bytes_per_mft_record);
            auto n = read_from_attribute(*reader, to_boot_geometry(info), mft_data,
                                         ntfs_core::ByteOffset{file_offset},
                                         std::span<std::byte>(record), cancellation);
            if (!n) {
                return base::Result<ParsedMftRecord>::failure(n.error());
            }
            if (n.value() != info.bytes_per_mft_record) {
                return base::Result<ParsedMftRecord>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
            }
        }
        return parse_mft_record_bytes(std::span<std::byte>(record), info.bytes_per_sector,
                                      record_number);
    }

    [[nodiscard]] base::Result<std::vector<std::uint64_t>>
    collect_extension_records(const ParsedMftRecord& base,
                              const base::CancellationToken cancellation) {
        std::vector<std::uint64_t> extensions;
        for (const auto& attr : base.attributes) {
            if (attr.type != detail::kAttrAttributeList) {
                continue;
            }
            std::vector<std::byte> list_bytes;
            if (!attr.non_resident) {
                list_bytes = attr.resident_data;
            } else {
                if (attr.data_size.value > kMaximumIndexRecordBytes) {
                    return base::Result<std::vector<std::uint64_t>>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
                }
                list_bytes.resize(static_cast<std::size_t>(attr.data_size.value));
                auto n = read_from_attribute(*reader, to_boot_geometry(info), attr,
                                             ntfs_core::ByteOffset{0},
                                             std::span<std::byte>(list_bytes), cancellation);
                if (!n) {
                    return base::Result<std::vector<std::uint64_t>>::failure(n.error());
                }
                list_bytes.resize(n.value());
            }
            auto parsed = parse_attribute_list(std::span<const std::byte>(list_bytes));
            if (!parsed) {
                return base::Result<std::vector<std::uint64_t>>::failure(parsed.error());
            }
            auto valid =
                ntfs_core::validate_attribute_list_entries(parsed.value(), base.record_number);
            if (!valid) {
                return base::Result<std::vector<std::uint64_t>>::failure(valid.error());
            }
            for (const auto& entry : parsed.value()) {
                if (entry.attribute_record.record_number != base.record_number) {
                    if (std::find(extensions.begin(), extensions.end(),
                                  entry.attribute_record.record_number) == extensions.end()) {
                        extensions.push_back(entry.attribute_record.record_number);
                    }
                }
                if (extensions.size() > kMaximumAttributeListDepth) {
                    return base::Result<std::vector<std::uint64_t>>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
                }
            }
        }
        return base::Result<std::vector<std::uint64_t>>::success(std::move(extensions));
    }

    [[nodiscard]] base::Result<FileDataLayout>
    load_layout(const NtfsFileReference reference, const base::CancellationToken cancellation) {
        if (auto cached = layout_cache.try_get(reference.record_number); cached.has_value()) {
            return base::Result<FileDataLayout>::success(*cached);
        }
        auto base = read_mft_record(reference.record_number, cancellation);
        if (!base) {
            return base::Result<FileDataLayout>::failure(base.error());
        }
        if (!base.value().in_use) {
            return base::Result<FileDataLayout>::failure(
                make_error(base::ErrorCode::kNotFound, "ntfs.entry_not_found"));
        }
        if (reference.sequence_number != 0 &&
            base.value().sequence_number != reference.sequence_number) {
            return base::Result<FileDataLayout>::failure(
                make_error(base::ErrorCode::kNotFound, "ntfs.entry_not_found"));
        }
        auto extension_ids = collect_extension_records(base.value(), cancellation);
        if (!extension_ids) {
            return base::Result<FileDataLayout>::failure(extension_ids.error());
        }
        std::vector<ParsedMftRecord> extensions;
        extensions.reserve(extension_ids.value().size());
        for (const auto id : extension_ids.value()) {
            auto ext = read_mft_record(id, cancellation);
            if (!ext) {
                return base::Result<FileDataLayout>::failure(ext.error());
            }
            if (!ext.value().in_use) {
                return base::Result<FileDataLayout>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
            }
            extensions.push_back(std::move(ext).value());
        }
        auto layout = build_file_layout(base.value(), extensions);
        if (!layout) {
            return layout;
        }
        layout_cache.put(reference.record_number, layout.value());
        return layout;
    }

    [[nodiscard]] base::Result<std::vector<NtfsEntry>>
    enumerate_directory(const FileDataLayout& layout, const base::CancellationToken cancellation) {
        std::vector<NtfsEntry> all;
        if (layout.has_index_root && !layout.index_root.non_resident) {
            const auto& root = layout.index_root.resident_data;
            if (root.size() < 32) {
                return base::Result<std::vector<NtfsEntry>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
            }
            const auto first_entry = read_u32(std::span<const std::byte>(root), 16);
            const auto total_size = read_u32(std::span<const std::byte>(root), 20);
            if (16U + first_entry > root.size() || 16U + total_size > root.size() ||
                first_entry > total_size) {
                return base::Result<std::vector<NtfsEntry>>::failure(
                    make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
            }
            auto page = parse_index_entries(std::span<const std::byte>(root).subspan(
                16 + first_entry, total_size - first_entry));
            if (!page) {
                return base::Result<std::vector<NtfsEntry>>::failure(page.error());
            }
            all.insert(all.end(), page.value().begin(), page.value().end());
        }

        if (layout.has_index_allocation) {
            auto alloc_page = enumerate_index_allocation(layout, cancellation);
            if (!alloc_page) {
                return alloc_page;
            }
            all.insert(all.end(), alloc_page.value().begin(), alloc_page.value().end());
        }

        std::vector<NtfsEntry> unique;
        unique.reserve(all.size());
        for (auto& entry : all) {
            auto existing =
                std::find_if(unique.begin(), unique.end(), [&](const NtfsEntry& candidate) {
                    return candidate.reference.record_number == entry.reference.record_number;
                });
            if (existing == unique.end()) {
                unique.push_back(std::move(entry));
            } else if (entry.name.size() > existing->name.size()) {
                *existing = std::move(entry);
            }
        }
        std::sort(unique.begin(), unique.end(), [](const NtfsEntry& left, const NtfsEntry& right) {
            if (left.is_directory != right.is_directory) {
                return left.is_directory && !right.is_directory;
            }
            return left.name < right.name;
        });
        return base::Result<std::vector<NtfsEntry>>::success(std::move(unique));
    }

    [[nodiscard]] base::Result<std::vector<NtfsEntry>>
    enumerate_index_allocation(const FileDataLayout& layout,
                               const base::CancellationToken cancellation) {
        const auto& alloc = layout.index_allocation;
        if (alloc.compressed) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.compressed_unsupported"));
        }
        const auto record_size = info.bytes_per_index_record;
        constexpr std::uint64_t kMaxIndexBufferCount = 1'000'000;
        if (record_size == 0 || alloc.data_size.value % record_size != 0) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
        }
        const auto buffer_count = alloc.data_size.value / record_size;
        if (buffer_count > kMaxIndexBufferCount || !layout.has_index_bitmap) {
            return base::Result<std::vector<NtfsEntry>>::failure(
                make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt"));
        }
        auto index_bitmap_bytes =
            load_index_bitmap(*reader, info, layout.index_bitmap, buffer_count, cancellation);
        if (!index_bitmap_bytes) {
            return base::Result<std::vector<NtfsEntry>>::failure(index_bitmap_bytes.error());
        }

        std::vector<NtfsEntry> all;
        std::uint64_t offset = 0;
        for (std::uint64_t buffer_index = 0; buffer_index < buffer_count; ++buffer_index) {
            if (cancellation.stop_requested()) {
                return base::Result<std::vector<NtfsEntry>>::failure(
                    make_error(base::ErrorCode::kCancelled, "ntfs.read_failed"));
            }
            auto in_use =
                ntfs_core::bitmap_bit_is_set(index_bitmap_bytes.value(), buffer_index);
            if (!in_use) {
                return base::Result<std::vector<NtfsEntry>>::failure(in_use.error());
            }
            if (!in_use.value()) {
                offset += record_size;
                continue;
            }
            std::vector<std::byte> record(record_size);
            auto n = read_from_attribute(*reader, to_boot_geometry(info), alloc,
                                         ntfs_core::ByteOffset{offset},
                                         std::span<std::byte>(record), cancellation);
            if (!n || n.value() < record_size) {
                return base::Result<std::vector<NtfsEntry>>::failure(
                    n ? make_error(base::ErrorCode::kCorruptData, "ntfs.index_corrupt")
                      : n.error());
            }
            auto page = parse_indx_buffer(std::span<std::byte>(record), info.bytes_per_sector);
            if (!page) {
                return base::Result<std::vector<NtfsEntry>>::failure(page.error());
            }
            all.insert(all.end(), page.value().begin(), page.value().end());
            offset += record_size;
        }
        return base::Result<std::vector<NtfsEntry>>::success(std::move(all));
    }
};

NtfsVolumeReader::NtfsVolumeReader(std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

NtfsVolumeReader::~NtfsVolumeReader() = default;

base::Result<std::unique_ptr<NtfsVolumeReader>>
NtfsVolumeReader::open(ports::IRandomAccessReader& reader,
                       const base::CancellationToken cancellation) {
    auto implementation = std::make_unique<Impl>();
    implementation->reader = &reader;

    std::vector<std::byte> boot(512);
    auto n = reader.read_at(0, std::span<std::byte>(boot), cancellation);
    if (!n) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(n.error());
    }
    if (n.value() < 512) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.invalid_boot_sector"));
    }
    auto geometry = parse_boot_sector(std::span<const std::byte>(boot));
    if (!geometry) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(geometry.error());
    }
    implementation->info = to_volume_info(geometry.value());

    auto mft_record = implementation->read_mft_record(0, cancellation);
    if (!mft_record) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(mft_record.error());
    }
    auto mft_layout = build_file_layout(mft_record.value(), {});
    if (!mft_layout || !mft_layout.value().has_unnamed_data) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
    }
    implementation->mft_data = std::move(mft_layout).value().unnamed_data;
    implementation->mft_ready = true;

    auto root = implementation->read_mft_record(kNtfsRootFileReference, cancellation);
    if (!root || !root.value().in_use) {
        return base::Result<std::unique_ptr<NtfsVolumeReader>>::failure(
            make_error(base::ErrorCode::kCorruptData, "ntfs.corrupt_mft_record"));
    }

    return base::Result<std::unique_ptr<NtfsVolumeReader>>::success(
        std::unique_ptr<NtfsVolumeReader>(new NtfsVolumeReader(std::move(implementation))));
}

const NtfsVolumeInfo& NtfsVolumeReader::volume_info() const noexcept {
    return implementation_->info;
}

base::Result<NtfsDirectoryPage> NtfsVolumeReader::list_directory(
    const NtfsFileReference directory, const std::uint32_t maximum_results,
    const std::optional<std::string>& continuation, const base::CancellationToken cancellation) {
    if (maximum_results == 0 || maximum_results > kMaximumDirectoryPage) {
        return base::Result<NtfsDirectoryPage>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.index_corrupt"));
    }
    auto skip = decode_continuation(continuation);
    if (!skip) {
        return base::Result<NtfsDirectoryPage>::failure(skip.error());
    }
    auto layout = implementation_->load_layout(directory, cancellation);
    if (!layout) {
        return base::Result<NtfsDirectoryPage>::failure(layout.error());
    }
    if (!layout.value().is_directory) {
        return base::Result<NtfsDirectoryPage>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.entry_not_found"));
    }
    auto all = implementation_->enumerate_directory(layout.value(), cancellation);
    if (!all) {
        return base::Result<NtfsDirectoryPage>::failure(all.error());
    }
    NtfsDirectoryPage page;
    const auto start = static_cast<std::size_t>(skip.value());
    if (start > all.value().size()) {
        return base::Result<NtfsDirectoryPage>::success(std::move(page));
    }
    const auto end =
        (std::min)(all.value().size(), start + static_cast<std::size_t>(maximum_results));
    page.items.assign(all.value().begin() + static_cast<std::ptrdiff_t>(start),
                      all.value().begin() + static_cast<std::ptrdiff_t>(end));
    if (end < all.value().size()) {
        auto token = encode_continuation(static_cast<std::uint32_t>(end));
        if (!token) {
            return base::Result<NtfsDirectoryPage>::failure(token.error());
        }
        page.continuation_token = std::move(token).value();
    }
    return base::Result<NtfsDirectoryPage>::success(std::move(page));
}

base::Result<NtfsEntry>
NtfsVolumeReader::describe_entry(const NtfsFileReference reference,
                                 const base::CancellationToken cancellation) {
    auto layout = implementation_->load_layout(reference, cancellation);
    if (!layout) {
        return base::Result<NtfsEntry>::failure(layout.error());
    }
    NtfsEntry entry;
    entry.reference = reference;
    if (reference.sequence_number == 0) {
        auto record = implementation_->read_mft_record(reference.record_number, cancellation);
        if (record) {
            entry.reference.sequence_number = record.value().sequence_number;
        }
    }
    entry.name = layout.value().best_name;
    entry.is_directory = layout.value().is_directory;
    entry.is_reparse = layout.value().is_reparse;
    entry.is_compressed = layout.value().is_compressed;
    entry.is_encrypted = layout.value().is_encrypted;
    entry.is_hidden = (layout.value().file_attributes & detail::kFileAttrHidden) != 0;
    entry.is_system = (layout.value().file_attributes & detail::kFileAttrSystem) != 0;
    entry.file_attributes = layout.value().file_attributes;
    entry.logical_size = layout.value().logical_size;
    entry.allocated_size = layout.value().allocated_size;
    entry.creation_time = layout.value().creation_time;
    entry.modification_time = layout.value().modification_time;
    entry.mft_change_time = layout.value().mft_change_time;
    entry.access_time = layout.value().access_time;
    return base::Result<NtfsEntry>::success(std::move(entry));
}

base::Result<std::size_t> NtfsVolumeReader::read_file(const NtfsFileReference reference,
                                                      const std::uint64_t offset,
                                                      const std::span<std::byte> destination,
                                                      const base::CancellationToken cancellation) {
    if (destination.size() > kMaximumStreamReadBytes) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.read_failed"));
    }
    auto layout = implementation_->load_layout(reference, cancellation);
    if (!layout) {
        return base::Result<std::size_t>::failure(layout.error());
    }
    if (layout.value().is_directory) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "ntfs.entry_not_found"));
    }
    if (layout.value().is_compressed) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.compressed_unsupported"));
    }
    if (layout.value().is_encrypted) {
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kUnsupportedVersion, "ntfs.efs_unsupported"));
    }
    if (!layout.value().has_unnamed_data) {
        if (offset == 0 && destination.empty()) {
            return base::Result<std::size_t>::success(0);
        }
        if (layout.value().logical_size == 0) {
            return base::Result<std::size_t>::success(0);
        }
        return base::Result<std::size_t>::failure(
            make_error(base::ErrorCode::kNotFound, "ntfs.entry_not_found"));
    }
    return read_from_attribute(*implementation_->reader, to_boot_geometry(implementation_->info),
                               layout.value().unnamed_data, ntfs_core::ByteOffset{offset},
                               destination, cancellation);
}

} // namespace aegra::adapters::ntfs
