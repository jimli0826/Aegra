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

[[nodiscard]] Json encode_stream(const contracts::FileStreamDesc& stream) {
    Json extents = Json::array();
    for (const auto& extent : stream.extents) {
        extents.push_back(encode_extent(extent));
    }
    // FI0/FI1 exact stream schema: main + content_storage local|parent; no ADS/sparse fields.
    return Json::object(
        {{"stream_index", stream.stream_index},
         {"stream_kind", static_cast<std::uint8_t>(stream.stream_kind)},
         {"name_encoding", static_cast<std::uint8_t>(stream.name.encoding)},
         {"name", encode_binary(stream.name.bytes)},
         {"logical_size", stream.logical_size},
         {"content_storage", static_cast<std::uint8_t>(stream.content_storage)},
         {"parent_stream_index", stream.parent_stream_index},
         {"extent_count", static_cast<std::uint32_t>(stream.extents.size())},
         {"extents", std::move(extents)}});
}

[[nodiscard]] base::Result<contracts::FileStreamDesc> decode_stream(const Json& value) {
    if (!value.is_object() || value.contains("allocated_ranges")) {
        return base::Result<contracts::FileStreamDesc>::failure(
            corrupt("file stream uses unsupported sparse/ADS fields"));
    }
    contracts::FileStreamDesc stream;
    stream.stream_index = value.at("stream_index").get<std::uint32_t>();
    stream.stream_kind =
        static_cast<contracts::FileStreamKind>(value.at("stream_kind").get<std::uint8_t>());
    stream.name.encoding =
        static_cast<contracts::NameEncoding>(value.at("name_encoding").get<std::uint8_t>());
    stream.name.bytes = decode_binary(value.at("name"));
    stream.logical_size = value.at("logical_size").get<std::uint64_t>();
    stream.content_storage =
        static_cast<contracts::FileContentStorage>(value.at("content_storage").get<std::uint8_t>());
    stream.parent_stream_index = value.at("parent_stream_index").get<std::uint32_t>();
    for (const auto& extent : value.at("extents")) {
        stream.extents.push_back(decode_extent(extent));
    }
    if (value.at("extent_count").get<std::uint32_t>() != stream.extents.size()) {
        return base::Result<contracts::FileStreamDesc>::failure(
            corrupt("file stream extent_count mismatch"));
    }
    return base::Result<contracts::FileStreamDesc>::success(std::move(stream));
}

[[nodiscard]] Json encode_identity(const contracts::StableFileIdentity& identity) {
    return Json::object({{"volume_identity",
                          encode_binary(std::vector<std::byte>(
                              reinterpret_cast<const std::byte*>(identity.volume_identity.data()),
                              reinterpret_cast<const std::byte*>(identity.volume_identity.data()) +
                                  identity.volume_identity.size()))},
                         {"file_id", encode_binary(std::vector<std::byte>(identity.file_id.begin(),
                                                                          identity.file_id.end()))}});
}

[[nodiscard]] base::Result<contracts::StableFileIdentity> decode_identity(const Json& value) {
    if (!value.is_object()) {
        return base::Result<contracts::StableFileIdentity>::failure(
            corrupt("stable_file_identity is invalid"));
    }
    contracts::StableFileIdentity identity;
    const auto volume = decode_binary(value.at("volume_identity"));
    identity.volume_identity.assign(reinterpret_cast<const char*>(volume.data()), volume.size());
    const auto file_id = decode_binary(value.at("file_id"));
    if (file_id.size() != identity.file_id.size()) {
        return base::Result<contracts::StableFileIdentity>::failure(
            corrupt("stable_file_identity file_id length is invalid"));
    }
    std::copy(file_id.begin(), file_id.end(), identity.file_id.begin());
    return base::Result<contracts::StableFileIdentity>::success(std::move(identity));
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
    auto validated = contracts::validate_file_entry_desc(entry);
    if (!validated) {
        return base::Result<std::vector<std::byte>>::failure(validated.error());
    }
    auto key = make_index_key(entry);
    if (!key) {
        return base::Result<std::vector<std::byte>>::failure(key.error());
    }
    Json streams = Json::array();
    for (const auto& stream : entry.streams) {
        streams.push_back(encode_stream(stream));
    }
    // FI0/FI1 exact entry schema: dir/file + stable identity + main stream (no hard_link_group).
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
        {"stable_file_identity", encode_identity(entry.stable_identity)},
        {"attributes", entry.attributes},
        {"flags", entry.flags},
        {"creation_time", entry.creation_time},
        {"access_time", entry.access_time},
        {"write_time", entry.write_time},
        {"change_time", entry.change_time},
        {"logical_size", entry.logical_size},
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
        if (!root.value().is_object() || root.value().contains("hard_link_group")) {
            return base::Result<contracts::FileEntryDesc>::failure(
                corrupt("file index leaf uses unsupported hard-link fields"));
        }
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
        {
            auto identity = decode_identity(root.value().at("stable_file_identity"));
            if (!identity) {
                return base::Result<contracts::FileEntryDesc>::failure(identity.error());
            }
            entry.stable_identity = std::move(identity).value();
        }
        entry.attributes = root.value().at("attributes").get<std::uint32_t>();
        entry.flags = root.value().at("flags").get<std::uint32_t>();
        entry.creation_time = root.value().at("creation_time").get<std::uint64_t>();
        entry.access_time = root.value().at("access_time").get<std::uint64_t>();
        entry.write_time = root.value().at("write_time").get<std::uint64_t>();
        entry.change_time = root.value().at("change_time").get<std::uint64_t>();
        entry.logical_size = root.value().at("logical_size").get<std::uint64_t>();
        for (const auto& stream : root.value().at("streams")) {
            auto decoded = decode_stream(stream);
            if (!decoded) {
                return base::Result<contracts::FileEntryDesc>::failure(decoded.error());
            }
            entry.streams.push_back(std::move(decoded).value());
        }
        if (root.value().at("stream_count").get<std::uint32_t>() != entry.streams.size()) {
            return base::Result<contracts::FileEntryDesc>::failure(
                corrupt("file index stream_count mismatch"));
        }
        entry.platform_metadata = decode_binary(root.value().at("platform"));
        auto key = make_index_key(entry);
        if (!key) {
            return base::Result<contracts::FileEntryDesc>::failure(key.error());
        }
        auto validated = contracts::validate_file_entry_desc(entry);
        if (!validated) {
            return base::Result<contracts::FileEntryDesc>::failure(
                corrupt("file index leaf entry schema is unsupported"));
        }
        // Reject reparse platform sections (removed in FI0).
        auto security =
            extract_platform_security_descriptor(entry.platform_metadata);
        if (!security) {
            return base::Result<contracts::FileEntryDesc>::failure(security.error());
        }
        return base::Result<contracts::FileEntryDesc>::success(std::move(entry));
    } catch (const Json::exception&) {
        return base::Result<contracts::FileEntryDesc>::failure(
            corrupt("file index leaf entry fields are invalid"));
    }
}

[[nodiscard]] Json encode_child_locator(const ChildPageLocator& child) {
    return Json::object({{"page_id", child.page_id}, {"offset", child.offset}});
}

[[nodiscard]] base::Result<ChildPageLocator> decode_child_locator(const Json& value) {
    try {
        ChildPageLocator child;
        child.page_id = value.at("page_id").get<std::uint64_t>();
        child.offset = value.at("offset").get<std::uint64_t>();
        if (child.page_id == 0 || child.offset == 0) {
            return base::Result<ChildPageLocator>::failure(
                invalid("internal index page child locator is invalid"));
        }
        return base::Result<ChildPageLocator>::success(child);
    } catch (const Json::exception&) {
        return base::Result<ChildPageLocator>::failure(
            corrupt("internal index page child locator is invalid"));
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
    for (const auto& child : body.children) {
        if (child.page_id == 0 || child.offset == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("internal index page child locator is invalid"));
        }
        children.push_back(encode_child_locator(child));
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
            auto locator = decode_child_locator(child);
            if (!locator) {
                return base::Result<InternalPageBody>::failure(locator.error());
            }
            body.children.push_back(locator.value());
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
make_index_root_digest_preimage(const std::string_view label, const std::uint64_t root_page_id,
                                const std::uint64_t page_count, const std::uint64_t primary_count,
                                const std::uint64_t secondary_count,
                                const std::span<const std::byte, 32> root_page_content_digest) {
    std::vector<std::byte> preimage;
    const auto* label_bytes = reinterpret_cast<const std::byte*>(label.data());
    preimage.insert(preimage.end(), label_bytes, label_bytes + label.size());
    write_le64(preimage, root_page_id);
    write_le64(preimage, page_count);
    write_le64(preimage, primary_count);
    write_le64(preimage, secondary_count);
    preimage.insert(preimage.end(), root_page_content_digest.begin(),
                    root_page_content_digest.end());
    return preimage;
}

std::vector<std::byte>
make_index_root_digest_preimage(const std::uint64_t root_page_id, const std::uint64_t page_count,
                                const std::uint64_t entry_count, const std::uint64_t stream_count,
                                const std::span<const std::byte, 32> root_page_content_digest) {
    return make_index_root_digest_preimage(kIndexRootDigestLabel, root_page_id, page_count,
                                           entry_count, stream_count, root_page_content_digest);
}

base::Result<std::vector<std::byte>>
encode_integer_internal_page_cbor(const IntegerInternalPageBody& body) {
    if (body.page_kind != kPageKindEntryIdInternal && body.page_kind != kPageKindStreamInternal &&
        body.page_kind != kPageKindChunkInternal) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("integer internal page kind is invalid"));
    }
    if (body.children.size() != body.keys.size() + 1 || body.keys.empty() ||
        body.keys.size() > kMaximumInternalKeysPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("integer internal page structure is invalid"));
    }
    for (std::size_t index = 1; index < body.keys.size(); ++index) {
        if (body.keys[index] <= body.keys[index - 1]) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("integer internal page keys are not sorted"));
        }
    }
    Json keys = Json::array();
    for (const auto key : body.keys) {
        keys.push_back(key);
    }
    Json children = Json::array();
    for (const auto& child : body.children) {
        if (child.page_id == 0 || child.offset == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("integer internal page child locator is invalid"));
        }
        children.push_back(encode_child_locator(child));
    }
    return dump_cbor(Json::object({{"page_kind", body.page_kind},
                                   {"keys", std::move(keys)},
                                   {"children", std::move(children)}}));
}

base::Result<IntegerInternalPageBody>
decode_integer_internal_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<IntegerInternalPageBody>::failure(root.error());
    }
    try {
        IntegerInternalPageBody body;
        body.page_kind = root.value().at("page_kind").get<std::uint16_t>();
        if (body.page_kind != kPageKindEntryIdInternal &&
            body.page_kind != kPageKindStreamInternal &&
            body.page_kind != kPageKindChunkInternal) {
            return base::Result<IntegerInternalPageBody>::failure(
                corrupt("integer internal page kind is invalid"));
        }
        for (const auto& key : root.value().at("keys")) {
            body.keys.push_back(key.get<std::uint64_t>());
        }
        for (const auto& child : root.value().at("children")) {
            auto locator = decode_child_locator(child);
            if (!locator) {
                return base::Result<IntegerInternalPageBody>::failure(locator.error());
            }
            body.children.push_back(locator.value());
        }
        if (body.children.size() != body.keys.size() + 1 || body.keys.empty()) {
            return base::Result<IntegerInternalPageBody>::failure(
                corrupt("integer internal page structure is invalid"));
        }
        for (std::size_t index = 1; index < body.keys.size(); ++index) {
            if (body.keys[index] <= body.keys[index - 1]) {
                return base::Result<IntegerInternalPageBody>::failure(
                    corrupt("integer internal page keys are not sorted"));
            }
        }
        return base::Result<IntegerInternalPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<IntegerInternalPageBody>::failure(
            corrupt("integer internal page fields are invalid"));
    }
}

base::Result<std::vector<std::byte>>
encode_entry_id_leaf_page_cbor(const EntryIdLeafPageBody& body) {
    if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("entry id leaf page record count is invalid"));
    }
    Json records = Json::array();
    for (std::size_t index = 0; index < body.records.size(); ++index) {
        const auto& rec = body.records[index];
        if (rec.entry_id == 0 || rec.page_id == 0 || rec.page_offset == 0 ||
            (rec.kind != 1 && rec.kind != 2)) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("entry id leaf record is invalid"));
        }
        if (index > 0 && rec.entry_id <= body.records[index - 1].entry_id) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("entry id leaf records are not sorted"));
        }
        records.push_back(Json::object({{"entry_id", rec.entry_id},
                                        {"page_id", rec.page_id},
                                        {"page_offset", rec.page_offset},
                                        {"slot", rec.slot},
                                        {"parent_entry_id", rec.parent_entry_id},
                                        {"kind", rec.kind}}));
    }
    return dump_cbor(Json::object({{"page_kind", kPageKindEntryIdLeaf},
                                   {"records", std::move(records)}}));
}

base::Result<EntryIdLeafPageBody>
decode_entry_id_leaf_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<EntryIdLeafPageBody>::failure(root.error());
    }
    try {
        if (root.value().at("page_kind").get<std::uint16_t>() != kPageKindEntryIdLeaf) {
            return base::Result<EntryIdLeafPageBody>::failure(
                corrupt("entry id leaf page kind is invalid"));
        }
        EntryIdLeafPageBody body;
        for (const auto& item : root.value().at("records")) {
            EntryIdIndexRecord rec;
            rec.entry_id = item.at("entry_id").get<std::uint64_t>();
            rec.page_id = item.at("page_id").get<std::uint64_t>();
            rec.page_offset = item.at("page_offset").get<std::uint64_t>();
            rec.slot = item.at("slot").get<std::uint32_t>();
            rec.parent_entry_id = item.at("parent_entry_id").get<std::uint64_t>();
            rec.kind = item.at("kind").get<std::uint8_t>();
            if (rec.entry_id == 0 || rec.page_id == 0 || rec.page_offset == 0 ||
                (rec.kind != 1 && rec.kind != 2)) {
                return base::Result<EntryIdLeafPageBody>::failure(
                    corrupt("entry id leaf record is invalid"));
            }
            if (!body.records.empty() && rec.entry_id <= body.records.back().entry_id) {
                return base::Result<EntryIdLeafPageBody>::failure(
                    corrupt("entry id leaf records are not sorted"));
            }
            body.records.push_back(rec);
        }
        if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
            return base::Result<EntryIdLeafPageBody>::failure(
                corrupt("entry id leaf page record count is invalid"));
        }
        return base::Result<EntryIdLeafPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<EntryIdLeafPageBody>::failure(
            corrupt("entry id leaf page fields are invalid"));
    }
}

base::Result<std::vector<std::byte>>
encode_stream_leaf_page_cbor(const StreamLeafPageBody& body) {
    if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("stream leaf page record count is invalid"));
    }
    Json records = Json::array();
    for (std::size_t index = 0; index < body.records.size(); ++index) {
        const auto& rec = body.records[index];
        if (rec.stream_index == 0 || rec.entry_id == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("stream leaf record is invalid"));
        }
        if (index > 0 && rec.stream_index <= body.records[index - 1].stream_index) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("stream leaf records are not sorted"));
        }
        records.push_back(Json::object({{"stream_index", rec.stream_index},
                                        {"entry_id", rec.entry_id},
                                        {"stream_slot", rec.stream_slot}}));
    }
    return dump_cbor(Json::object(
        {{"page_kind", kPageKindStreamLeaf}, {"records", std::move(records)}}));
}

base::Result<StreamLeafPageBody>
decode_stream_leaf_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<StreamLeafPageBody>::failure(root.error());
    }
    try {
        if (root.value().at("page_kind").get<std::uint16_t>() != kPageKindStreamLeaf) {
            return base::Result<StreamLeafPageBody>::failure(
                corrupt("stream leaf page kind is invalid"));
        }
        StreamLeafPageBody body;
        for (const auto& item : root.value().at("records")) {
            StreamIndexRecord rec;
            rec.stream_index = item.at("stream_index").get<std::uint32_t>();
            rec.entry_id = item.at("entry_id").get<std::uint64_t>();
            rec.stream_slot = item.at("stream_slot").get<std::uint32_t>();
            if (rec.stream_index == 0 || rec.entry_id == 0) {
                return base::Result<StreamLeafPageBody>::failure(
                    corrupt("stream leaf record is invalid"));
            }
            if (!body.records.empty() && rec.stream_index <= body.records.back().stream_index) {
                return base::Result<StreamLeafPageBody>::failure(
                    corrupt("stream leaf records are not sorted"));
            }
            body.records.push_back(rec);
        }
        if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
            return base::Result<StreamLeafPageBody>::failure(
                corrupt("stream leaf page record count is invalid"));
        }
        return base::Result<StreamLeafPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<StreamLeafPageBody>::failure(
            corrupt("stream leaf page fields are invalid"));
    }
}

base::Result<std::vector<std::byte>>
encode_chunk_leaf_page_cbor(const ChunkLeafPageBody& body) {
    if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
        return base::Result<std::vector<std::byte>>::failure(
            invalid("chunk leaf page record count is invalid"));
    }
    Json records = Json::array();
    for (std::size_t index = 0; index < body.records.size(); ++index) {
        const auto& rec = body.records[index];
        // chunk_index may be 0 (Writer assigns dense ids from 0).
        if (rec.record_offset == 0 || rec.payload_offset == 0) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("chunk leaf record is invalid"));
        }
        if (index > 0 && rec.chunk_index <= body.records[index - 1].chunk_index) {
            return base::Result<std::vector<std::byte>>::failure(
                invalid("chunk leaf records are not sorted"));
        }
        records.push_back(Json::object({{"chunk_index", rec.chunk_index},
                                        {"record_offset", rec.record_offset},
                                        {"payload_offset", rec.payload_offset},
                                        {"payload_size", rec.payload_size},
                                        {"block_entry_count", rec.block_entry_count}}));
    }
    return dump_cbor(
        Json::object({{"page_kind", kPageKindChunkLeaf}, {"records", std::move(records)}}));
}

base::Result<ChunkLeafPageBody>
decode_chunk_leaf_page_cbor(const std::span<const std::byte> encoded) {
    auto root = load_cbor(encoded);
    if (!root) {
        return base::Result<ChunkLeafPageBody>::failure(root.error());
    }
    try {
        if (root.value().at("page_kind").get<std::uint16_t>() != kPageKindChunkLeaf) {
            return base::Result<ChunkLeafPageBody>::failure(
                corrupt("chunk leaf page kind is invalid"));
        }
        ChunkLeafPageBody body;
        for (const auto& item : root.value().at("records")) {
            ChunkIndexRecord rec;
            rec.chunk_index = item.at("chunk_index").get<std::uint64_t>();
            rec.record_offset = item.at("record_offset").get<std::uint64_t>();
            rec.payload_offset = item.at("payload_offset").get<std::uint64_t>();
            rec.payload_size = item.at("payload_size").get<std::uint64_t>();
            rec.block_entry_count = item.at("block_entry_count").get<std::uint32_t>();
            if (rec.record_offset == 0 || rec.payload_offset == 0) {
                return base::Result<ChunkLeafPageBody>::failure(
                    corrupt("chunk leaf record is invalid"));
            }
            if (!body.records.empty() && rec.chunk_index <= body.records.back().chunk_index) {
                return base::Result<ChunkLeafPageBody>::failure(
                    corrupt("chunk leaf records are not sorted"));
            }
            body.records.push_back(rec);
        }
        if (body.records.empty() || body.records.size() > kMaximumLeafEntriesPerPage) {
            return base::Result<ChunkLeafPageBody>::failure(
                corrupt("chunk leaf page record count is invalid"));
        }
        return base::Result<ChunkLeafPageBody>::success(std::move(body));
    } catch (const Json::exception&) {
        return base::Result<ChunkLeafPageBody>::failure(
            corrupt("chunk leaf page fields are invalid"));
    }
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
        // FI0: reparse section (historical tag 2) and any other section are unsupported.
        if (tag_id == 2 || critical) {
            return base::Result<std::vector<std::byte>>::failure(
                corrupt("platform metadata has unsupported section"));
        }
    }
    return base::Result<std::vector<std::byte>>::success(std::move(security));
}

} // namespace aegra::format::file_index
