#include "aegra/adapters/personal_archive/personal_archive.h"

#include "personal_archive_preamble.h"
#include "personal_file_archive_lazy_index.h"
#include "win32_input_file.h"

#include "aegra/adapters/compression_zstd/zstd_codec.h"
#include "aegra/base/error.h"
#include "aegra/contracts/file_set.h"
#include "aegra/format/personal_archive.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive {
namespace {

namespace archive = format::personal_archive;
namespace lazy = lazy_index;

[[nodiscard]] base::Error error(base::ErrorCode code, std::string message) {
    return {code, std::move(message)};
}

/// Decrypts chunk payload then expands RAW / zstd COMPRESSED BlockEntry to logical bytes.
[[nodiscard]] base::Result<std::vector<std::byte>>
materialize_file_block(detail::Win32InputFile& input, lazy::OpenedFileArchive& state,
                       const lazy::StreamChunkLocator& locator, const std::size_t entry_index) {
    if (entry_index >= locator.entries.size()) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "stream extent block index is invalid"));
    }
    const auto& block = locator.entries[entry_index];
    auto payload_bytes =
        lazy::read_exact(input, locator.payload_offset, locator.header.payload_size);
    if (!payload_bytes) {
        return base::Result<std::vector<std::byte>>::failure(payload_bytes.error());
    }
    std::vector<std::byte> stored = std::move(payload_bytes).value();
    if (state.encryption_enabled) {
        auto aad = lazy::make_file_chunk_aad(state.part_header,
                                             archive::kBlockEntrySize + stored.size(),
                                             locator.header, locator.entries);
        if (!aad) {
            return base::Result<std::vector<std::byte>>::failure(aad.error());
        }
        auto plain = state.payload_cipher->unprotect(stored, aad.value(), locator.header.payload_nonce,
                                                     locator.header.payload_authentication_tag);
        if (!plain) {
            return base::Result<std::vector<std::byte>>::failure(plain.error());
        }
        stored = std::move(plain).value();
    }
    if (block.data_offset_or_reference > stored.size() ||
        block.stored_size > stored.size() - block.data_offset_or_reference) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "file stream block exceeds chunk payload"));
    }
    const auto block_stored =
        std::span<const std::byte>(stored).subspan(
            static_cast<std::size_t>(block.data_offset_or_reference), block.stored_size);
    if (block.flags == archive::kBlockFlagRaw) {
        if (block.stored_size != block.logical_size) {
            return base::Result<std::vector<std::byte>>::failure(
                error(base::ErrorCode::kCorruptData, "raw file stream block size is invalid"));
        }
        return base::Result<std::vector<std::byte>>::success(
            std::vector<std::byte>(block_stored.begin(), block_stored.end()));
    }
    if (block.flags != archive::kBlockFlagCompressed || block.logical_size == 0) {
        return base::Result<std::vector<std::byte>>::failure(
            error(base::ErrorCode::kCorruptData, "file stream block flags are invalid"));
    }
    const auto max_out = static_cast<std::size_t>(
        (std::max)(block.logical_size, state.preamble.header.block_size));
    return compression_zstd::decompress(block_stored, static_cast<std::size_t>(block.logical_size),
                                        max_out);
}

} // namespace

struct PersonalFileArchiveReader::Impl final {
    /// Mutable so const accessors can complete deferred root auth (M6 / ADR-0019).
    mutable lazy::OpenedFileArchive archive;
    ArchiveIdentity identity;
    mutable detail::Win32InputFile input;
};

PersonalFileArchiveReader::PersonalFileArchiveReader(
    std::unique_ptr<Impl> implementation) noexcept
    : implementation_(std::move(implementation)) {}

PersonalFileArchiveReader::~PersonalFileArchiveReader() = default;

base::Result<void> PersonalFileArchiveReader::ensure_index_loaded() const {
    return lazy::ensure_roots(implementation_->input, implementation_->archive);
}

base::Result<void> PersonalFileArchiveReader::for_each_entry_in_leaf_order(
    const base::CancellationToken cancellation,
    const std::function<base::Result<void>(const contracts::FileEntryDesc&)>& visitor) const {
    if (!visitor) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive entry visitor is required"));
    }
    auto ready = ensure_index_loaded();
    if (!ready) {
        return ready;
    }
    return lazy::for_each_entry_in_leaf_order(implementation_->input, implementation_->archive,
                                              cancellation, visitor);
}

base::Result<void>
PersonalFileArchiveReader::verify_index_and_parent_graph(
    const base::CancellationToken cancellation) const {
    auto ready = ensure_index_loaded();
    if (!ready) {
        return ready;
    }
    return lazy::verify_entry_id_index_and_parent_graph(implementation_->input,
                                                        implementation_->archive, cancellation);
}

base::Result<std::unique_ptr<PersonalFileArchiveReader>>
PersonalFileArchiveReader::open(const ArchiveOpenRequest& request) {
    if (request.source.empty()) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(
            error(base::ErrorCode::kInvalidArgument, "file archive source is required"));
    }
    detail::Win32InputFile input;
    if (auto opened = input.open(request.source); !opened) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(
            error(base::ErrorCode::kNotFound, "file archive was not found"));
    }
    auto file_size = lazy::read_stream_size(input);
    if (!file_size) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(
            file_size.error());
    }
    auto state = lazy::open_file_archive_state(input, request, file_size.value());
    if (!state) {
        return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::failure(state.error());
    }
    auto implementation = std::make_unique<Impl>();
    implementation->identity.file_uuid = state.value().preamble.header.file_uuid;
    implementation->identity.backup_set_uuid = state.value().preamble.header.backup_set_uuid;
    implementation->identity.parent_uuid = state.value().preamble.header.parent_uuid;
    implementation->identity.backup_type =
        detail::archive_backup_type(state.value().preamble.header);
    implementation->identity.block_size = state.value().preamble.header.block_size;
    implementation->input = std::move(input);
    implementation->archive = std::move(state).value();
    return base::Result<std::unique_ptr<PersonalFileArchiveReader>>::success(
        std::unique_ptr<PersonalFileArchiveReader>(
            new PersonalFileArchiveReader(std::move(implementation))));
}

const format::Manifest& PersonalFileArchiveReader::manifest() const noexcept {
    return implementation_->archive.preamble.manifest;
}

const ArchiveIdentity& PersonalFileArchiveReader::identity() const noexcept {
    return implementation_->identity;
}

std::string PersonalFileArchiveReader::index_root_digest() const {
    return lazy::digest_to_hex(implementation_->archive.footer.index_root_digest);
}

std::uint64_t PersonalFileArchiveReader::entry_count() const noexcept {
    return implementation_->archive.footer.entry_count;
}

std::uint64_t PersonalFileArchiveReader::stream_count() const noexcept {
    return implementation_->archive.footer.stream_count;
}

base::Result<ports::FileEntryPage>
PersonalFileArchiveReader::list_children(const std::uint64_t parent_entry_id,
                                         const std::uint32_t maximum_results,
                                         const std::optional<std::string>& continuation_token,
                                         const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<ports::FileEntryPage>::failure(
            error(base::ErrorCode::kCancelled, "list cancelled"));
    }
    if (maximum_results == 0) {
        return base::Result<ports::FileEntryPage>::failure(
            error(base::ErrorCode::kInvalidArgument, "maximum_results must be positive"));
    }
    auto ready = ensure_index_loaded();
    if (!ready) {
        return base::Result<ports::FileEntryPage>::failure(ready.error());
    }
    auto start = lazy::parse_token(continuation_token);
    if (!start) {
        return base::Result<ports::FileEntryPage>::failure(start.error());
    }
    return lazy::list_children(implementation_->input, implementation_->archive, parent_entry_id,
                               maximum_results, start.value(), cancellation);
}

base::Result<contracts::FileEntryDesc>
PersonalFileArchiveReader::describe_entry(const std::uint64_t entry_id,
                                          const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<contracts::FileEntryDesc>::failure(
            error(base::ErrorCode::kCancelled, "describe cancelled"));
    }
    auto ready = ensure_index_loaded();
    if (!ready) {
        return base::Result<contracts::FileEntryDesc>::failure(ready.error());
    }
    return lazy::load_entry_by_id(implementation_->input, implementation_->archive, entry_id);
}

base::Result<FileStreamOwnerView>
PersonalFileArchiveReader::describe_stream_owner(const std::uint32_t stream_index,
                                                 const base::CancellationToken cancellation) const {
    if (cancellation.stop_requested()) {
        return base::Result<FileStreamOwnerView>::failure(
            error(base::ErrorCode::kCancelled, "stream describe cancelled"));
    }
    if (stream_index == 0) {
        return base::Result<FileStreamOwnerView>::failure(
            error(base::ErrorCode::kInvalidArgument, "file stream index is invalid"));
    }
    auto ready = ensure_index_loaded();
    if (!ready) {
        return base::Result<FileStreamOwnerView>::failure(ready.error());
    }
    auto found =
        lazy::lookup_stream_record(implementation_->input, implementation_->archive, stream_index);
    if (!found) {
        return base::Result<FileStreamOwnerView>::failure(found.error());
    }
    auto entry = lazy::load_entry_by_id(implementation_->input, implementation_->archive,
                                        found.value().entry_id);
    if (!entry) {
        return base::Result<FileStreamOwnerView>::failure(entry.error());
    }
    if (found.value().stream_slot >= entry.value().streams.size()) {
        return base::Result<FileStreamOwnerView>::failure(
            error(base::ErrorCode::kCorruptData, "file stream slot is invalid"));
    }
    FileStreamOwnerView view;
    view.identity = entry.value().stable_identity;
    view.entry_kind = entry.value().kind;
    view.stream = entry.value().streams[found.value().stream_slot];
    if (view.stream.stream_index != stream_index) {
        return base::Result<FileStreamOwnerView>::failure(
            error(base::ErrorCode::kCorruptData, "file stream index locator mismatch"));
    }
    return base::Result<FileStreamOwnerView>::success(std::move(view));
}

base::Result<std::size_t>
PersonalFileArchiveReader::read_stream(const ports::FileStreamReadRequest& request,
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
    auto owner = describe_stream_owner(request.stream_index, cancellation);
    if (!owner) {
        return base::Result<std::size_t>::failure(owner.error());
    }
    const auto* stream = &owner.value().stream;
    if (stream->content_storage == contracts::FileContentStorage::kParent) {
        return base::Result<std::size_t>::failure(error(
            base::ErrorCode::kInvalidArgument,
            "parent stream requires file chain reader"));
    }
    if (stream->content_storage != contracts::FileContentStorage::kLocal) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kCorruptData, "file stream content_storage is invalid"));
    }
    if (request.offset >= stream->logical_size) {
        return base::Result<std::size_t>::success(0);
    }
    const auto readable =
        (std::min)(request.size, static_cast<std::uint64_t>(stream->logical_size - request.offset));
    std::size_t written = 0;
    const auto end = request.offset + readable;
    for (const auto& extent : stream->extents) {
        if (cancellation.stop_requested()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCancelled, "stream read cancelled"));
        }
        const auto extent_end = extent.file_offset + extent.logical_size;
        if (extent_end <= request.offset || extent.file_offset >= end) {
            continue;
        }
        auto locator = lazy::load_chunk_locator(implementation_->input, implementation_->archive,
                                                extent.chunk_index);
        if (!locator) {
            return base::Result<std::size_t>::failure(locator.error());
        }
        if (locator.value().header.source_index != stream->stream_index) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "file stream chunk owner mismatch"));
        }
        auto logical = materialize_file_block(implementation_->input, implementation_->archive,
                                              locator.value(), extent.block_entry_index);
        if (!logical) {
            return base::Result<std::size_t>::failure(logical.error());
        }
        if (logical.value().size() != extent.logical_size) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "stream extent size mismatches block"));
        }
        const auto copy_begin = (std::max)(request.offset, extent.file_offset);
        const auto copy_end = (std::min)(end, extent_end);
        const auto local_begin = static_cast<std::size_t>(copy_begin - extent.file_offset);
        const auto local_size = static_cast<std::size_t>(copy_end - copy_begin);
        if (local_begin + local_size > logical.value().size()) {
            return base::Result<std::size_t>::failure(
                error(base::ErrorCode::kCorruptData, "stream extent exceeds chunk payload"));
        }
        const auto dest_offset = static_cast<std::size_t>(copy_begin - request.offset);
        std::copy_n(logical.value().begin() + static_cast<std::ptrdiff_t>(local_begin), local_size,
                    destination.begin() + static_cast<std::ptrdiff_t>(dest_offset));
        written = (std::max)(written, dest_offset + local_size);
    }
    return base::Result<std::size_t>::success(written);
}

} // namespace aegra::adapters::personal_archive
