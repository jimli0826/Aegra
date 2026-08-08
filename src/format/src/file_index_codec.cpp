#include "aegra/format/file_index.h"

#include "aegra/base/error.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <string>
#include <utility>

namespace aegra::format::file_index {
namespace {

using Json = nlohmann::json;

[[nodiscard]] base::Error corrupt(std::string message) {
    return {base::ErrorCode::kCorruptData, std::move(message)};
}

[[nodiscard]] base::Error invalid(std::string message) {
    return {base::ErrorCode::kInvalidArgument, std::move(message)};
}

void write_le64(std::vector<std::byte>& out, const std::uint64_t value) {
    for (std::size_t index = 0; index < 8; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] Json encode_binary(const std::vector<std::byte>& bytes) {
    return Json::binary(std::vector<std::uint8_t>(
        reinterpret_cast<const std::uint8_t*>(bytes.data()),
        reinterpret_cast<const std::uint8_t*>(bytes.data()) + bytes.size()));
}

[[nodiscard]] std::vector<std::byte> decode_binary(const Json& value) {
    const auto& binary = value.get_binary();
    return {reinterpret_cast<const std::byte*>(binary.data()),
            reinterpret_cast<const std::byte*>(binary.data()) + binary.size()};
}

[[nodiscard]] Json encode_name(const contracts::EncodedName& name) {
    return Json::object({{"encoding", static_cast<std::uint8_t>(name.encoding)},
                         {"bytes", encode_binary(name.bytes)}});
}

[[nodiscard]] contracts::EncodedName decode_name(const Json& value) {
    contracts::EncodedName name;
    name.encoding = static_cast<contracts::NameEncoding>(value.at("encoding").get<std::uint8_t>());
    name.bytes = decode_binary(value.at("bytes"));
    return name;
}

[[nodiscard]] Json encode_extent(const contracts::FileStreamExtentDesc& extent) {
    return Json::object({{"chunk_index", extent.chunk_index},
                         {"block_entry_index", extent.block_entry_index},
                         {"file_offset", extent.file_offset},
                         {"logical_size", extent.logical_size}});
}

[[nodiscard]] contracts::FileStreamExtentDesc decode_extent(const Json& value) {
    contracts::FileStreamExtentDesc extent;
    extent.chunk_index = value.at("chunk_index").get<std::uint64_t>();
    extent.block_entry_index = value.at("block_entry_index").get<std::uint32_t>();
    extent.file_offset = value.at("file_offset").get<std::uint64_t>();
    extent.logical_size = value.at("logical_size").get<std::uint64_t>();
    return extent;
}

[[nodiscard]] Json encode_range(const contracts::FileAllocatedRangeDesc& range) {
    return Json::object({{"offset", range.offset}, {"length", range.length}});
}

[[nodiscard]] contracts::FileAllocatedRangeDesc decode_range(const Json& value) {
    contracts::FileAllocatedRangeDesc range;
    range.offset = value.at("offset").get<std::uint64_t>();
    range.length = value.at("length").get<std::uint64_t>();
    return range;
}

[[nodiscard]] Json encode_stream(const contracts::FileStreamDesc& stream) {
    Json extents = Json::array();
    for (const auto& extent : stream.extents) {
        extents.push_back(encode_extent(extent));
    }
    Json ranges = Json::array();
    for (const auto& range : stream.allocated_ranges) {
        ranges.push_back(encode_range(range));
    }
    return Json::object({{"stream_index", stream.stream_index},
                         {"stream_kind", static_cast<std::uint8_t>(stream.stream_kind)},
                         {"name_encoding", static_cast<std::uint8_t>(stream.name.encoding)},
                         {"name", encode_binary(stream.name.bytes)},
                         {"logical_size", stream.logical_size},
                         {"extent_count", static_cast<std::uint32_t>(stream.extents.size())},
                         {"extents", std::move(extents)},
                         {"allocated_ranges", std::move(ranges)}});
}

[[nodiscard]] contracts::FileStreamDesc decode_stream(const Json& value) {
    contracts::FileStreamDesc stream;
    stream.stream_index = value.at("stream_index").get<std::uint32_t>();
    stream.stream_kind =
        static_cast<contracts::FileStreamKind>(value.at("stream_kind").get<std::uint8_t>());
    stream.name.encoding =
        static_cast<contracts::NameEncoding>(value.at("name_encoding").get<std::uint8_t>());
    stream.name.bytes = decode_binary(value.at("name"));
    stream.logical_size = value.at("logical_size").get<std::uint64_t>();
    for (const auto& extent : value.at("extents")) {
        stream.extents.push_back(decode_extent(extent));
    }
    if (value.contains("allocated_ranges")) {
        for (const auto& range : value.at("allocated_ranges")) {
            stream.allocated_ranges.push_back(decode_range(range));
        }
    }
    return stream;
}

[[nodiscard]] Json encode_key(const IndexKey& key) {
    return Json::object({{"parent_entry_id", key.parent_entry_id},
                         {"name_encoding", key.name_encoding},
                         {"name_bytes", encode_binary(key.name_bytes)},
                         {"entry_id", key.entry_id}});
}

[[nodiscard]] IndexKey decode_key(const Json& value) {
    IndexKey key;
    key.parent_entry_id = value.at("parent_entry_id").get<std::uint64_t>();
    key.name_encoding = value.at("name_encoding").get<std::uint8_t>();
    key.name_bytes = decode_binary(value.at("name_bytes"));
    key.entry_id = value.at("entry_id").get<std::uint64_t>();
    return key;
}

[[nodiscard]] base::Result<std::vector<std::byte>> dump_cbor(const Json& root) {
    try {
        auto bytes = Json::to_cbor(root);
        return base::Result<std::vector<std::byte>>::success(std::vector<std::byte>(
            reinterpret_cast<const std::byte*>(bytes.data()),
            reinterpret_cast<const std::byte*>(bytes.data()) + bytes.size()));
    } catch (const Json::exception&) {
        return base::Result<std::vector<std::byte>>::failure(corrupt("file index CBOR encode failed"));
    }
}

[[nodiscard]] base::Result<Json> load_cbor(const std::span<const std::byte> encoded) {
    try {
        return base::Result<Json>::success(Json::from_cbor(
            reinterpret_cast<const std::uint8_t*>(encoded.data()),
            reinterpret_cast<const std::uint8_t*>(encoded.data()) + encoded.size()));
    } catch (const Json::exception&) {
        return base::Result<Json>::failure(corrupt("file index CBOR decode failed"));
    }
}

} // namespace

int compare_index_keys(const IndexKey& left, const IndexKey& right) noexcept {
    if (left.parent_entry_id != right.parent_entry_id) {
        return left.parent_entry_id < right.parent_entry_id ? -1 : 1;
    }
    if (left.name_encoding != right.name_encoding) {
        return left.name_encoding < right.name_encoding ? -1 : 1;
    }
    const auto common = (std::min)(left.name_bytes.size(), right.name_bytes.size());
    const auto cmp = std::memcmp(left.name_bytes.data(), right.name_bytes.data(), common);
    if (cmp != 0) {
        return cmp < 0 ? -1 : 1;
    }
    if (left.name_bytes.size() != right.name_bytes.size()) {
        return left.name_bytes.size() < right.name_bytes.size() ? -1 : 1;
    }
    if (left.entry_id != right.entry_id) {
        return left.entry_id < right.entry_id ? -1 : 1;
    }
    return 0;
}

base::Result<IndexKey> make_index_key(const contracts::FileEntryDesc& entry) {
    if (entry.entry_id == 0 || entry.name.encoding != contracts::NameEncoding::kWindowsUtf16Le ||
        entry.name.bytes.size() < contracts::kMinimumNameUtf16LeBytes ||
        entry.name.bytes.size() > contracts::kMaximumNameUtf16LeBytes ||
        (entry.name.bytes.size() % 2U) != 0) {
        return base::Result<IndexKey>::failure(invalid("file index entry key is invalid"));
    }
    IndexKey key;
    key.parent_entry_id = entry.parent_entry_id;
    key.name_encoding = kNameEncodingWindowsUtf16Le;
    key.name_bytes = entry.name.bytes;
    key.entry_id = entry.entry_id;
    return base::Result<IndexKey>::success(std::move(key));
}

base::Result<std::vector<std::byte>> encode_leaf_entry_cbor(const contracts::FileEntryDesc& entry) {
    auto key = make_index_key(entry);
    if (!key) {
        return base::Result<std::vector<std::byte>>::failure(key.error());
    }
    Json streams = Json::array();
    for (const auto& stream : entry.streams) {
        streams.push_back(encode_stream(stream));
    }
    const Json root = {
        {"entry_id", entry.entry_id},
        {"parent_entry_id", entry.parent_entry_id},
        {"kind", static_cast<std::uint8_t>(entry.kind)},
        {"name_encoding", static_cast<std::uint8_t>(entry.name.encoding)},
        {"name", encode_binary(entry.name.bytes)},
        {"selection_id",
         encode_binary(std::vector<std::byte>(
             reinterpret_cast<const std::byte*>(entry.selection_id.data()),
             reinterpret_cast<const std::byte*>(entry.selection_id.data()) +
                 entry.selection_id.size()))},
        {"attributes", entry.attributes},
        {"flags", entry.flags},
        {"creation_time", entry.creation_time},
        {"access_time", entry.access_time},
        {"write_time", entry.write_time},
        {"change_time", entry.change_time},
        {"logical_size", entry.logical_size},
        {"hard_link_group", entry.hard_link_group},
        {"stream_count", static_cast<std::uint32_t>(entry.streams.size())},
        {"streams", std::move(streams)},
        {"platform", encode_binary(entry.platform_metadata)},
    };
    return dump_cbor(root);
}

base::Result<contracts::FileEntryDesc>
decode_leaf_entry_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<contracts::FileEntryDesc>::failure(root.error());
    }
    try {
        contracts::FileEntryDesc entry;
        entry.entry_id = root.value().at("entry_id").get<std::uint64_t>();
        entry.parent_entry_id = root.value().at("parent_entry_id").get<std::uint64_t>();
        entry.kind =
            static_cast<contracts::FileEntryKind>(root.value().at("kind").get<std::uint8_t>());
        entry.name.encoding = static_cast<contracts::NameEncoding>(
            root.value().at("name_encoding").get<std::uint8_t>());
        entry.name.bytes = decode_binary(root.value().at("name"));
        {
            const auto selection = decode_binary(root.value().at("selection_id"));
            entry.selection_id.assign(reinterpret_cast<const char*>(selection.data()),
                                      selection.size());
        }
        entry.attributes = root.value().at("attributes").get<std::uint32_t>();
        entry.flags = root.value().at("flags").get<std::uint32_t>();
        entry.creation_time = root.value().at("creation_time").get<std::uint64_t>();
        entry.access_time = root.value().at("access_time").get<std::uint64_t>();
        entry.write_time = root.value().at("write_time").get<std::uint64_t>();
        entry.change_time = root.value().at("change_time").get<std::uint64_t>();
        entry.logical_size = root.value().at("logical_size").get<std::uint64_t>();
        entry.hard_link_group = root.value().at("hard_link_group").get<std::uint64_t>();
        for (const auto& stream : root.value().at("streams")) {
            entry.streams.push_back(decode_stream(stream));
        }
        entry.platform_metadata = decode_binary(root.value().at("platform"));
        auto key = make_index_key(entry);
        if (!key) {
            return base::Result<contracts::FileEntryDesc>::failure(key.error());
        }
        return base::Result<contracts::FileEntryDesc>::success(std::move(entry));
    } catch (const Json::exception&) {
        return base::Result<contracts::FileEntryDesc>::failure(
            corrupt("file index leaf entry fields are invalid"));
    }
}

base::Result<std::vector<std::byte>> encode_internal_page_cbor(const InternalPageBody& body) {
    if (body.children.size() != body.keys.size() + 1 || body.keys.empty() ||
        body.keys.size() > kMaximumInternalKeysPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("internal index page structure is invalid"));
    }
    for (std::size_t index = 1; index < body.keys.size(); ++index) {
        if (compare_index_keys(body.keys[index - 1], body.keys[index]) >= 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("internal index page keys are not sorted"));
        }
    }
    Json keys = Json::array();
    for (const auto& key : body.keys) {
        keys.push_back(encode_key(key));
    }
    Json children = Json::array();
    for (const auto child : body.children) {
        if (child == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("internal index page child id is invalid"));
        }
        children.push_back(child);
    }
    return dump_cbor(Json::object(
        {{"page_kind", 2}, {"keys", std::move(keys)}, {"children", std::move(children)}}));
}

base::Result<InternalPageBody> decode_internal_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<InternalPageBody>::failure(root.error());
    }
    try {
        if (root.value().at("page_kind").get<std::uint16_t>() != 2) {
            return base::Result<InternalPageBody>::failure(
                corrupt("internal index page kind is invalid"));
        }
        InternalPageBody body;
        for (const auto& key : root.value().at("keys")) {
            body.keys.push_back(decode_key(key));
        }
        for (const auto& child : root.value().at("children")) {
            body.children.push_back(child.get<std::uint64_t>());
        }
        if (body.children.size() != body.keys.size() + 1 || body.keys.empty()) {
            return base::Result<InternalPageBody>::failure(
                corrupt("internal index page structure is invalid"));
        }
        for (std::size_t index = 1; index < body.keys.size(); ++index) {
            if (compare_index_keys(body.keys[index - 1], body.keys[index]) >= 0) {
                return base::Result<InternalPageBody>::failure(
                    corrupt("internal index page keys are not sorted"));
            }
        }
        return base::Result<InternalPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<InternalPageBody>::failure(
            corrupt("internal index page fields are invalid"));
    }
}

base::Result<std::vector<std::byte>> encode_leaf_page_cbor(const LeafPageBody& body) {
    if (body.entries.empty() || body.entries.size() > kMaximumLeafEntriesPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("leaf index page entry count is invalid"));
    }
    auto sorted = validate_leaf_page_sorted(body);
    if (!sorted) {
        return base::Result<std::vector<std::byte>>::failure(sorted.error());
    }
    Json entries = Json::array();
    for (const auto& entry : body.entries) {
        auto encoded = encode_leaf_entry_cbor(entry);
        if (!encoded) {
            return base::Result<std::vector<std::byte>>::failure(encoded.error());
        }
        entries.push_back(encode_binary(encoded.value()));
    }
    return dump_cbor(Json::object({{"page_kind", 1}, {"entries", std::move(entries)}}));
}

base::Result<LeafPageBody> decode_leaf_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<LeafPageBody>::failure(root.error());
    }
    try {
        if (root.value().at("page_kind").get<std::uint16_t>() != 1) {
            return base::Result<LeafPageBody>::failure(corrupt("leaf index page kind is invalid"));
        }
        LeafPageBody body;
        for (const auto& item : root.value().at("entries")) {
            auto entry = decode_leaf_entry_cbor(decode_binary(item));
            if (!entry) {
                return base::Result<LeafPageBody>::failure(entry.error());
            }
            body.entries.push_back(std::move(entry).value());
        }
        auto sorted = validate_leaf_page_sorted(body);
        if (!sorted) {
            return base::Result<LeafPageBody>::failure(sorted.error());
        }
        return base::Result<LeafPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<LeafPageBody>::failure(corrupt("leaf index page fields are invalid"));
    }
}

base::Result<void> validate_leaf_page_sorted(const LeafPageBody& body) {
    if (body.entries.empty()) {
        return base::Result<void>::failure(invalid("leaf page is empty"));
    }
    auto previous = make_index_key(body.entries.front());
    if (!previous) {
        return base::Result<void>::failure(previous.error());
    }
    for (std::size_t index = 1; index < body.entries.size(); ++index) {
        auto current = make_index_key(body.entries[index]);
        if (!current) {
            return base::Result<void>::failure(current.error());
        }
        if (compare_index_keys(previous.value(), current.value()) >= 0) {
            return base::Result<void>::failure(corrupt("leaf index page keys are not sorted"));
        }
        previous = std::move(current);
    }
    return base::Result<void>::success();
}

std::vector<std::byte>
make_index_root_digest_preimage(const std::uint64_t root_page_id, const std::uint64_t page_count,
                                const std::uint64_t entry_count, const std::uint64_t stream_count,
                                const std::span<const std::byte, 32> root_page_content_digest) {
    std::vector<std::byte> preimage;
    const auto* label = reinterpret_cast<const std::byte*>(kIndexRootDigestLabel);
    const auto label_size = std::strlen(kIndexRootDigestLabel);
    preimage.insert(preimage.end(), label, label + label_size);
    write_le64(preimage, root_page_id);
    write_le64(preimage, page_count);
    write_le64(preimage, entry_count);
    write_le64(preimage, stream_count);
    preimage.insert(preimage.end(), root_page_content_digest.begin(),
                    root_page_content_digest.end());
    return preimage;
}

namespace {

void append_le16(std::vector<std::byte>& out, const std::uint16_t value) {
    out.push_back(static_cast<std::byte>(value & 0xFFU));
    out.push_back(static_cast<std::byte>((value >> 8U) & 0xFFU));
}

void append_le32(std::vector<std::byte>& out, const std::uint32_t value) {
    for (std::size_t index = 0; index < 4; ++index) {
        out.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xFFU));
    }
}

[[nodiscard]] bool read_le16(const std::span<const std::byte> bytes, const std::size_t offset,
                             std::uint16_t& value) noexcept {
    if (offset + 2 > bytes.size()) {
        return false;
    }
    value = static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset])) |
            (static_cast<std::uint16_t>(std::to_integer<unsigned>(bytes[offset + 1])) << 8U);
    return true;
}

[[nodiscard]] bool read_le32(const std::span<const std::byte> bytes, const std::size_t offset,
                             std::uint32_t& value) noexcept {
    if (offset + 4 > bytes.size()) {
        return false;
    }
    value = 0;
    for (std::size_t index = 0; index < 4; ++index) {
        value |= static_cast<std::uint32_t>(std::to_integer<unsigned>(bytes[offset + index]))
                 << (index * 8U);
    }
    return true;
}

} // namespace

base::Result<std::vector<std::byte>>
encode_platform_security_envelope(const std::span<const std::byte> self_relative_security_descriptor) {
    if (self_relative_security_descriptor.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("security descriptor is empty"));
    }
    // header(8) + section header(8) + payload
    const auto total = 8U + 8U + self_relative_security_descriptor.size();
    if (total > contracts::kMaximumPlatformMetadataBytes) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("file_source.metadata_limit"));
    }
    std::vector<std::byte> out;
    out.reserve(total);
    append_le16(out, kPlatformEnvelopeVersion);
    append_le16(out, 0);
    append_le32(out, 0);
    append_le16(out, kPlatformSectionSecurity);
    append_le16(out, 0);
    append_le32(out, static_cast<std::uint32_t>(self_relative_security_descriptor.size()));
    out.insert(out.end(), self_relative_security_descriptor.begin(),
               self_relative_security_descriptor.end());
    return base::Result<std::vector<std::byte>>::success(std::move(out));
}

base::Result<std::vector<std::byte>>
extract_platform_security_descriptor(const std::span<const std::byte> platform_metadata) {
    if (platform_metadata.empty()) {
        return base::Result<std::vector<std::byte>>::success({});
    }
    if (platform_metadata.size() > contracts::kMaximumPlatformMetadataBytes ||
        platform_metadata.size() < 8) {
        return base::Result<std::vector<std::byte>>::failure(
            corrupt("platform metadata envelope is invalid"));
    }
    std::uint16_t version = 0;
    std::uint16_t reserved = 0;
    std::uint32_t flags = 0;
    if (!read_le16(platform_metadata, 0, version) || !read_le16(platform_metadata, 2, reserved) ||
        !read_le32(platform_metadata, 4, flags)) {
        return base::Result<std::vector<std::byte>>::failure(
            corrupt("platform metadata envelope is invalid"));
    }
    if (version != kPlatformEnvelopeVersion || reserved != 0) {
        return base::Result<std::vector<std::byte>>::failure(
            corrupt("platform metadata envelope version is unsupported"));
    }
    std::vector<std::byte> security;
    std::size_t offset = 8;
    while (offset < platform_metadata.size()) {
        if (offset + 8 > platform_metadata.size()) {
            return base::Result<std::vector<std::byte>>::failure(
                corrupt("platform metadata section is truncated"));
        }
        std::uint16_t tag = 0;
        std::uint16_t section_reserved = 0;
        std::uint32_t length = 0;
        if (!read_le16(platform_metadata, offset, tag) ||
            !read_le16(platform_metadata, offset + 2, section_reserved) ||
            !read_le32(platform_metadata, offset + 4, length)) {
            return base::Result<std::vector<std::byte>>::failure(
                corrupt("platform metadata section is invalid"));
        }
        offset += 8;
        if (section_reserved != 0 || length > platform_metadata.size() - offset) {
            return base::Result<std::vector<std::byte>>::failure(
                corrupt("platform metadata section is invalid"));
        }
        const auto body = platform_metadata.subspan(offset, length);
        offset += length;
        const auto tag_id = static_cast<std::uint16_t>(tag & ~kPlatformTagCriticalMask);
        const bool critical = (tag & kPlatformTagCriticalMask) != 0;
        if (tag_id == kPlatformSectionSecurity) {
            if (body.empty()) {
                return base::Result<std::vector<std::byte>>::failure(
                    corrupt("platform security section is empty"));
            }
            security.assign(body.begin(), body.end());
            continue;
        }
        if (critical) {
            return base::Result<std::vector<std::byte>>::failure(
                corrupt("platform metadata has unknown critical section"));
        }
    }
    return base::Result<std::vector<std::byte>>::success(std::move(security));
}

} // namespace aegra::format::file_index
