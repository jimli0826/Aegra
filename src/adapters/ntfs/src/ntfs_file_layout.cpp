#include "ntfs_internal.h"

#include <utility>

namespace aegra::adapters::ntfs::detail {
namespace {

[[nodiscard]] bool prefer_file_name(const std::uint8_t namespace_type,
                                    const std::uint8_t existing_namespace) noexcept {
    const auto score = [](const std::uint8_t ns) noexcept -> int {
        if (ns == kFileNameWin32AndDos || ns == kFileNameWin32) {
            return 2;
        }
        if (ns == 0) {
            return 1;
        }
        return 0;
    };
    return score(namespace_type) >= score(existing_namespace);
}

} // namespace

base::Result<FileDataLayout> build_file_layout(const ParsedMftRecord& record,
                                               const std::vector<ParsedMftRecord>& extensions) {
    FileDataLayout layout;
    layout.is_directory = record.is_directory;
    std::uint8_t best_name_ns = 0xFF;

    auto consume = [&](const ParsedMftRecord& source) -> base::Result<void> {
        for (const auto& attr : source.attributes) {
            if (attr.type == kAttrStandardInformation && !attr.non_resident &&
                attr.resident_data.size() >= 48) {
                layout.creation_time = read_u64(attr.resident_data, 0);
                layout.modification_time = read_u64(attr.resident_data, 8);
                layout.mft_change_time = read_u64(attr.resident_data, 16);
                layout.access_time = read_u64(attr.resident_data, 24);
                layout.file_attributes = read_u32(attr.resident_data, 32);
                layout.is_reparse = (layout.file_attributes & kFileAttrReparse) != 0;
                layout.is_compressed = (layout.file_attributes & kFileAttrCompressed) != 0 ||
                                       attr.compressed;
                layout.is_encrypted = (layout.file_attributes & kFileAttrEncrypted) != 0 ||
                                      attr.encrypted;
            } else if (attr.type == kAttrFileName && !attr.non_resident &&
                       attr.resident_data.size() >= 66) {
                const auto name_length = std::to_integer<std::uint8_t>(attr.resident_data[64]);
                const auto name_ns = std::to_integer<std::uint8_t>(attr.resident_data[65]);
                if (66U + static_cast<std::size_t>(name_length) * 2U > attr.resident_data.size()) {
                    return base::Result<void>::failure(
                        make_error(base::ErrorCode::kCorruptData, "ntfs.attribute_out_of_bounds"));
                }
                if (layout.best_name.empty() || prefer_file_name(name_ns, best_name_ns)) {
                    best_name_ns = name_ns;
                    layout.best_name.assign(
                        reinterpret_cast<const char16_t*>(attr.resident_data.data() + 66),
                        name_length);
                    const auto parent = read_u64(attr.resident_data, 0);
                    layout.parent_record = parent & 0x0000FFFFFFFFFFFFULL;
                    layout.parent_sequence = static_cast<std::uint16_t>(parent >> 48);
                }
            } else if (attr.type == kAttrData && attr.name.empty()) {
                layout.has_unnamed_data = true;
                layout.unnamed_data = attr;
                layout.logical_size = attr.data_size.value;
                layout.allocated_size = attr.allocated_size.value;
                layout.is_compressed = layout.is_compressed || attr.compressed;
                layout.is_encrypted = layout.is_encrypted || attr.encrypted;
            } else if (attr.type == kAttrIndexRoot &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_root = true;
                layout.index_root = attr;
            } else if (attr.type == kAttrIndexAllocation &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_allocation = true;
                layout.index_allocation = attr;
            } else if (attr.type == kAttrBitmap &&
                       (attr.name == u"$I30" || attr.name.empty())) {
                layout.has_index_bitmap = true;
                layout.index_bitmap = attr;
            }
        }
        return base::Result<void>::success();
    };

    auto base = consume(record);
    if (!base) {
        return base::Result<FileDataLayout>::failure(base.error());
    }
    for (const auto& extension : extensions) {
        auto ext = consume(extension);
        if (!ext) {
            return base::Result<FileDataLayout>::failure(ext.error());
        }
    }
    if ((layout.file_attributes & kFileAttrDirectory) != 0) {
        layout.is_directory = true;
    }
    return base::Result<FileDataLayout>::success(std::move(layout));
}

} // namespace aegra::adapters::ntfs::detail
