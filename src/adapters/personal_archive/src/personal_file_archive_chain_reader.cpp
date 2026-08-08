#include "aegra/adapters/personal_archive/personal_archive.h"

#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/format/manifest.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

[[nodiscard]] bool is_zero_uuid(const std::array<std::byte, 16>& value) noexcept {
    return std::all_of(value.begin(), value.end(),
                       [](const std::byte item) { return item == std::byte{0}; });
}

[[nodiscard]] bool same_fingerprint(const format::FileSetBaseline& left,
                                    const format::FileSetBaseline& right) noexcept {
    return left.fingerprint_algorithm == right.fingerprint_algorithm &&
           left.selection_fingerprint == right.selection_fingerprint;
}

[[nodiscard]] base::Result<std::vector<std::unique_ptr<PersonalFileArchiveReader>>>
open_file_layers(const ArchiveChainOpenRequest& request) {
    if (request.layers.empty() || request.maximum_chain_depth == 0 ||
        request.layers.size() > request.maximum_chain_depth ||
        request.layers.size() > contracts::kMaximumFileChainDepth) {
        return base::Result<std::vector<std::unique_ptr<PersonalFileArchiveReader>>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive chain request is invalid"));
    }
    std::vector<std::unique_ptr<PersonalFileArchiveReader>> layers;
    layers.reserve(request.layers.size());
    for (std::size_t index = 0; index < request.layers.size(); ++index) {
        auto layer_request = request.layers[index];
        // Tip needs Index for browse; ancestors defer maps until stream resolve (M6 / L31).
        layer_request.index_load = (index + 1 == request.layers.size())
                                       ? FileArchiveIndexLoad::kEager
                                       : FileArchiveIndexLoad::kDeferred;
        auto layer = PersonalFileArchiveReader::open(layer_request);
        if (!layer) {
            return base::Result<std::vector<std::unique_ptr<PersonalFileArchiveReader>>>::failure(
                layer.error());
        }
        layers.push_back(std::move(layer).value());
    }
    return base::Result<std::vector<std::unique_ptr<PersonalFileArchiveReader>>>::success(
        std::move(layers));
}

[[nodiscard]] base::Result<void>
validate_file_layer_link(const PersonalFileArchiveReader& parent,
                         const PersonalFileArchiveReader& child) {
    const auto& parent_id = parent.identity();
    const auto& child_id = child.identity();
    if (child_id.backup_type != format::BackupType::kIncremental) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain layer must be incremental"));
    }
    if (is_zero_uuid(child_id.parent_uuid) || child_id.parent_uuid != parent_id.file_uuid) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain parent_uuid mismatch"));
    }
    if (child_id.backup_set_uuid != parent_id.backup_set_uuid || is_zero_uuid(child_id.backup_set_uuid)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain backup_set mismatch"));
    }
    if (parent.manifest().content_kind != format::kManifestContentKindFileSet ||
        child.manifest().content_kind != format::kManifestContentKindFileSet) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain content_kind mismatch"));
    }
    if (!same_fingerprint(parent.manifest().file_set_baseline, child.manifest().file_set_baseline)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain selection fingerprint mismatch"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
validate_file_chain(const std::vector<std::unique_ptr<PersonalFileArchiveReader>>& layers) {
    const auto& root = layers.front()->identity();
    if (root.backup_type != format::BackupType::kFull || !is_zero_uuid(root.parent_uuid)) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain must begin with a full backup"));
    }
    if (layers.front()->manifest().content_kind != format::kManifestContentKindFileSet) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "file archive chain content_kind mismatch"));
    }
    std::vector<std::array<std::byte, 16>> seen;
    seen.reserve(layers.size());
    for (std::size_t index = 0; index < layers.size(); ++index) {
        const auto& identity = layers[index]->identity().file_uuid;
        if (is_zero_uuid(identity) ||
            std::find(seen.begin(), seen.end(), identity) != seen.end()) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kConflict, "file archive chain contains a repeated UUID"));
        }
        seen.push_back(identity);
        if (index == 0) {
            continue;
        }
        auto link = validate_file_layer_link(*layers[index - 1], *layers[index]);
        if (!link) {
            return link;
        }
    }
    return base::Result<void>::success();
}

struct ResolvedLocalStream final {
    std::size_t layer_index{0};
    std::uint32_t stream_index{0};
    std::uint64_t logical_size{0};
};

[[nodiscard]] base::Result<ResolvedLocalStream>
resolve_to_local(const std::vector<std::unique_ptr<PersonalFileArchiveReader>>& layers,
                 const std::size_t tip_layer_index, const std::uint32_t tip_stream_index,
                 const base::CancellationToken& cancellation) {
    if (tip_layer_index >= layers.size() || tip_stream_index == 0) {
        return base::Result<ResolvedLocalStream>::failure(
            error(base::ErrorCode::kInvalidArgument, "file chain resolve request is invalid"));
    }
    std::size_t layer_index = tip_layer_index;
    std::uint32_t stream_index = tip_stream_index;
    FileStreamOwnerView expected{};
    bool have_expected = false;
    std::unordered_set<std::uint64_t> visited;
    visited.reserve(layers.size());
    for (std::uint32_t hop = 0; hop < contracts::kMaximumFileChainDepth; ++hop) {
        if (cancellation.stop_requested()) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCancelled, "file chain resolve cancelled"));
        }
        const auto visit_key =
            (static_cast<std::uint64_t>(layer_index) << 32U) | static_cast<std::uint64_t>(stream_index);
        if (!visited.insert(visit_key).second) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCorruptData, "file chain parent stream cycle"));
        }
        auto owner = layers[layer_index]->describe_stream_owner(stream_index, cancellation);
        if (!owner) {
            return base::Result<ResolvedLocalStream>::failure(owner.error());
        }
        if (have_expected) {
            if (owner.value().identity != expected.identity ||
                owner.value().stream.stream_kind != expected.stream.stream_kind ||
                owner.value().stream.logical_size != expected.stream.logical_size) {
                return base::Result<ResolvedLocalStream>::failure(error(
                    base::ErrorCode::kCorruptData, "file chain parent stream identity mismatch"));
            }
        } else {
            expected = owner.value();
            have_expected = true;
        }
        if (owner.value().stream.content_storage == contracts::FileContentStorage::kLocal) {
            if (owner.value().stream.parent_stream_index != 0) {
                return base::Result<ResolvedLocalStream>::failure(
                    error(base::ErrorCode::kCorruptData, "local stream carries parent_stream_index"));
            }
            ResolvedLocalStream resolved;
            resolved.layer_index = layer_index;
            resolved.stream_index = stream_index;
            resolved.logical_size = owner.value().stream.logical_size;
            return base::Result<ResolvedLocalStream>::success(resolved);
        }
        if (owner.value().stream.content_storage != contracts::FileContentStorage::kParent) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCorruptData, "file stream content_storage is invalid"));
        }
        if (owner.value().stream.parent_stream_index == 0 || !owner.value().stream.extents.empty()) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCorruptData, "parent stream reference is invalid"));
        }
        if (layer_index == 0) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCorruptData, "parent stream past full root"));
        }
        const auto& child_identity = layers[layer_index]->identity();
        const auto& parent_identity = layers[layer_index - 1]->identity();
        if (child_identity.parent_uuid != parent_identity.file_uuid) {
            return base::Result<ResolvedLocalStream>::failure(
                error(base::ErrorCode::kCorruptData, "file chain parent layer mismatch"));
        }
        stream_index = owner.value().stream.parent_stream_index;
        --layer_index;
        expected = owner.value();
        expected.stream.content_storage = contracts::FileContentStorage::kLocal;
        expected.stream.parent_stream_index = 0;
        expected.stream.extents.clear();
    }
    return base::Result<ResolvedLocalStream>::failure(
        error(base::ErrorCode::kCorruptData, "file chain parent depth exceeded"));
}

[[nodiscard]] base::Result<std::uint64_t>
read_full_stream(ports::IFileRecoveryPointReader& reader, const std::uint32_t stream_index,
                 const std::uint64_t logical_size, const std::size_t budget,
                 const base::CancellationToken& cancellation) {
    if (logical_size == 0) {
        return base::Result<std::uint64_t>::success(0);
    }
    if (budget == 0) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file chain verify budget is invalid"));
    }
    std::uint64_t verified = 0;
    std::vector<std::byte> buffer(budget);
    while (verified < logical_size) {
        if (cancellation.stop_requested()) {
            return base::Result<std::uint64_t>::failure(
                error(base::ErrorCode::kCancelled, "file chain verify cancelled"));
        }
        const auto remaining = logical_size - verified;
        const auto request_size =
            static_cast<std::uint64_t>((std::min)(static_cast<std::uint64_t>(budget), remaining));
        ports::FileStreamReadRequest request;
        request.stream_index = stream_index;
        request.offset = verified;
        request.size = request_size;
        auto read = reader.read_stream(
            request, std::span<std::byte>(buffer.data(), static_cast<std::size_t>(request_size)),
            cancellation);
        if (!read) {
            return base::Result<std::uint64_t>::failure(read.error());
        }
        if (read.value() == 0) {
            return base::Result<std::uint64_t>::failure(
                error(base::ErrorCode::kCorruptData, "file stream ended before logical size"));
        }
        if (static_cast<std::uint64_t>(read.value()) > remaining) {
            return base::Result<std::uint64_t>::failure(
                error(base::ErrorCode::kCorruptData, "file stream read exceeded logical size"));
        }
        verified += static_cast<std::uint64_t>(read.value());
    }
    return base::Result<std::uint64_t>::success(verified);
}

[[nodiscard]] base::Result<void>
accumulate_verified_bytes(std::uint64_t& total, const std::uint64_t verified) {
    if (total > (std::numeric_limits<std::uint64_t>::max)() - verified) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCorruptData, "file chain verify byte count overflow"));
    }
    total += verified;
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
verify_entry_local_streams(PersonalFileArchiveReader& layer, const contracts::FileEntryDesc& entry,
                           const std::size_t budget, const base::CancellationToken& cancellation,
                           std::unordered_set<std::uint32_t>& seen, std::uint64_t& total) {
    for (const auto& stream : entry.streams) {
        if (!seen.insert(stream.stream_index).second) {
            continue;
        }
        if (stream.content_storage == contracts::FileContentStorage::kParent) {
            if (stream.parent_stream_index == 0 || !stream.extents.empty()) {
                return base::Result<void>::failure(
                    error(base::ErrorCode::kCorruptData, "parent stream reference is invalid"));
            }
            continue;
        }
        if (stream.content_storage != contracts::FileContentStorage::kLocal) {
            return base::Result<void>::failure(
                error(base::ErrorCode::kCorruptData, "file stream content_storage is invalid"));
        }
        auto verified =
            read_full_stream(layer, stream.stream_index, stream.logical_size, budget, cancellation);
        if (!verified) {
            return base::Result<void>::failure(verified.error());
        }
        auto added = accumulate_verified_bytes(total, verified.value());
        if (!added) {
            return added;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<void>
verify_entry_tip_streams(PersonalFileArchiveChainReader& chain, const contracts::FileEntryDesc& entry,
                         const std::size_t budget, const base::CancellationToken& cancellation,
                         std::unordered_set<std::uint32_t>& seen, std::uint64_t& total) {
    for (const auto& stream : entry.streams) {
        if (!seen.insert(stream.stream_index).second) {
            continue;
        }
        auto verified =
            read_full_stream(chain, stream.stream_index, stream.logical_size, budget, cancellation);
        if (!verified) {
            return base::Result<void>::failure(verified.error());
        }
        auto added = accumulate_verified_bytes(total, verified.value());
        if (!added) {
            return added;
        }
    }
    return base::Result<void>::success();
}

[[nodiscard]] base::Result<std::uint64_t>
verify_layer_local_payloads(PersonalFileArchiveReader& layer, const std::size_t budget,
                            const base::CancellationToken& cancellation) {
    std::uint64_t total = 0;
    std::unordered_set<std::uint32_t> seen;
    auto walk = layer.for_each_entry_in_leaf_order(
        cancellation, [&](const contracts::FileEntryDesc& entry) {
            return verify_entry_local_streams(layer, entry, budget, cancellation, seen, total);
        });
    if (!walk) {
        return base::Result<std::uint64_t>::failure(walk.error());
    }
    return base::Result<std::uint64_t>::success(total);
}

[[nodiscard]] base::Result<std::uint64_t>
verify_tip_streams(PersonalFileArchiveChainReader& chain, PersonalFileArchiveReader& tip,
                   const std::size_t budget, const base::CancellationToken& cancellation) {
    std::uint64_t total = 0;
    std::unordered_set<std::uint32_t> seen;
    auto walk = tip.for_each_entry_in_leaf_order(
        cancellation, [&](const contracts::FileEntryDesc& entry) {
            return verify_entry_tip_streams(chain, entry, budget, cancellation, seen, total);
        });
    if (!walk) {
        return base::Result<std::uint64_t>::failure(walk.error());
    }
    if (seen.size() != chain.stream_count()) {
        return base::Result<std::uint64_t>::failure(
            error(base::ErrorCode::kCorruptData, "file chain tip stream count mismatch"));
    }
    return base::Result<std::uint64_t>::success(total);
}

} // namespace

struct PersonalFileArchiveChainReader::Impl final {
    std::vector<std::unique_ptr<PersonalFileArchiveReader>> layers;
};

PersonalFileArchiveChainReader::PersonalFileArchiveChainReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalFileArchiveChainReader::~PersonalFileArchiveChainReader() = default;

base::Result<std::unique_ptr<PersonalFileArchiveChainReader>>
PersonalFileArchiveChainReader::open(const ArchiveChainOpenRequest& request) {
    auto layers = open_file_layers(request);
    if (!layers) {
        return base::Result<std::unique_ptr<PersonalFileArchiveChainReader>>::failure(layers.error());
    }
    auto valid = validate_file_chain(layers.value());
    if (!valid) {
        return base::Result<std::unique_ptr<PersonalFileArchiveChainReader>>::failure(valid.error());
    }
    auto implementation = std::make_unique<Impl>();
    implementation->layers = std::move(layers).value();
    return base::Result<std::unique_ptr<PersonalFileArchiveChainReader>>::success(
        std::unique_ptr<PersonalFileArchiveChainReader>(
            new PersonalFileArchiveChainReader(std::move(implementation))));
}

std::size_t PersonalFileArchiveChainReader::layer_count() const noexcept {
    return implementation_->layers.size();
}

const PersonalFileArchiveReader&
PersonalFileArchiveChainReader::layer_at(const std::size_t index) const {
    return *implementation_->layers.at(index);
}

const format::Manifest& PersonalFileArchiveChainReader::tip_manifest() const noexcept {
    return implementation_->layers.back()->manifest();
}

const ArchiveIdentity& PersonalFileArchiveChainReader::tip_identity() const noexcept {
    return implementation_->layers.back()->identity();
}

std::string PersonalFileArchiveChainReader::index_root_digest() const {
    return implementation_->layers.back()->index_root_digest();
}

std::string PersonalFileArchiveChainReader::chain_generation_digest() const {
    std::string digest;
    for (std::size_t index = 0; index < implementation_->layers.size(); ++index) {
        if (index != 0) {
            digest.push_back('+');
        }
        digest.append(implementation_->layers[index]->index_root_digest());
    }
    return digest;
}

std::uint64_t PersonalFileArchiveChainReader::entry_count() const noexcept {
    return implementation_->layers.back()->entry_count();
}

std::uint64_t PersonalFileArchiveChainReader::stream_count() const noexcept {
    return implementation_->layers.back()->stream_count();
}

base::Result<ports::FileEntryPage>
PersonalFileArchiveChainReader::list_children(const std::uint64_t parent_entry_id,
                                              const std::uint32_t maximum_results,
                                              const std::optional<std::string>& continuation_token,
                                              const base::CancellationToken cancellation) {
    return implementation_->layers.back()->list_children(parent_entry_id, maximum_results,
                                                         continuation_token, cancellation);
}

base::Result<contracts::FileEntryDesc>
PersonalFileArchiveChainReader::describe_entry(const std::uint64_t entry_id,
                                               const base::CancellationToken cancellation) {
    return implementation_->layers.back()->describe_entry(entry_id, cancellation);
}

base::Result<void>
PersonalFileArchiveChainReader::resolve_stream_reference(
    const std::uint32_t stream_index, const base::CancellationToken cancellation) const {
    if (stream_index == 0) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream index is invalid"));
    }
    const auto tip_index = implementation_->layers.size() - 1;
    auto resolved =
        resolve_to_local(implementation_->layers, tip_index, stream_index, cancellation);
    if (!resolved) {
        return base::Result<void>::failure(resolved.error());
    }
    return base::Result<void>::success();
}

base::Result<std::size_t>
PersonalFileArchiveChainReader::read_stream(const ports::FileStreamReadRequest& request,
                                            const std::span<std::byte> destination,
                                            const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kCancelled, "stream read cancelled"));
    }
    if (request.stream_index == 0 || request.size == 0 || destination.size() < request.size) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream read request is invalid"));
    }
    const auto tip_index = implementation_->layers.size() - 1;
    auto resolved =
        resolve_to_local(implementation_->layers, tip_index, request.stream_index, cancellation);
    if (!resolved) {
        return base::Result<std::size_t>::failure(resolved.error());
    }
    if (request.offset >= resolved.value().logical_size) {
        return base::Result<std::size_t>::success(0);
    }
    ports::FileStreamReadRequest local_request = request;
    local_request.stream_index = resolved.value().stream_index;
    return implementation_->layers[resolved.value().layer_index]->read_stream(local_request,
                                                                              destination,
                                                                              cancellation);
}

base::Result<FileChainVerifyResult>
PersonalFileArchiveChainReader::verify_recoverability(const std::size_t memory_budget_bytes,
                                                      const base::CancellationToken cancellation) {
    if (memory_budget_bytes == 0) {
        return base::Result<FileChainVerifyResult>::failure(
            error(base::ErrorCode::kInvalidArgument, "file chain verify budget is invalid"));
    }
    FileChainVerifyResult result;
    result.layer_count = implementation_->layers.size();
    result.tip_entry_count = entry_count();
    result.tip_stream_count = stream_count();
    for (auto& layer : implementation_->layers) {
        auto graph = layer->verify_index_and_parent_graph(cancellation);
        if (!graph) {
            return base::Result<FileChainVerifyResult>::failure(graph.error());
        }
        auto local = verify_layer_local_payloads(*layer, memory_budget_bytes, cancellation);
        if (!local) {
            return base::Result<FileChainVerifyResult>::failure(local.error());
        }
        if (result.local_payload_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - local.value()) {
            return base::Result<FileChainVerifyResult>::failure(
                error(base::ErrorCode::kCorruptData, "file chain verify byte count overflow"));
        }
        result.local_payload_bytes += local.value();
    }
    auto tip = verify_tip_streams(*this, *implementation_->layers.back(), memory_budget_bytes,
                                  cancellation);
    if (!tip) {
        return base::Result<FileChainVerifyResult>::failure(tip.error());
    }
    result.tip_resolved_bytes = tip.value();
    return base::Result<FileChainVerifyResult>::success(result);
}

} // namespace aegra::adapters::personal_archive
