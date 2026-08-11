#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive.h"
#include "aegra/format/personal_archive_sidecar.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/file_backup_session.h"
#include "aegra/ports/file_recovery_point.h"
#include "aegra/ports/random_access.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string_view>
#include <vector>

namespace aegra::adapters::personal_archive {

namespace detail {
class BlockWorkerPool;
}

struct ArchiveKdfParameters final {
    std::uint64_t opslimit{3};
    std::uint64_t memlimit_bytes{256ULL * 1024ULL * 1024ULL};
};

struct ArchiveCreateRequest final {
    std::filesystem::path destination;
    const format::Manifest& manifest;
    /// Required when encryption_enabled; empty when encryption is off.
    std::string_view password;
    /// When false, metadata and payloads are stored without AEAD (no password).
    bool encryption_enabled{false};
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::uint32_t block_size{0};
    std::uint32_t chunk_size{0};
    std::uint64_t split_size_bytes{0};
    ArchiveKdfParameters kdf_parameters;
    std::filesystem::path parent_source;
    // Views are consumed by create(); the session copies only its own password into secure memory.
    std::string_view parent_password;
    /// volume_set: enable same-chunk DEDUP (ADR-0022). Default true; frozen by Schedule.
    bool deduplication_enabled{true};
};

/// When to authenticate Index roots after Header/Footer (L31 / ADR-0019 / M6).
enum class FileArchiveIndexLoad : std::uint8_t {
    /// Open authenticates Footer roots immediately (single-layer open default; O(1) pages).
    kEager = 1,
    /// Open keeps only preamble/footer/ciphers; root auth on first Index/stream access.
    /// Chain open uses this for non-tip layers so browse does not pay per-ancestor root I/O.
    kDeferred = 2,
};

struct ArchiveOpenRequest final {
    std::filesystem::path source;
    std::string_view password;
    std::uint64_t maximum_metadata_size{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_payload_size{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_logical_size{1024ULL * 1024ULL * 1024ULL};
    std::uint32_t maximum_split_parts{10'000};
    FileArchiveIndexLoad index_load{FileArchiveIndexLoad::kEager};
    /// Enables a bounded depth-one payload prefetch for sequential restore readers.
    bool sequential_payload_prefetch{false};
};

struct ArchiveSidecar final {
    std::uint32_t block_size{0};
    std::array<std::byte, 16> file_uuid{};
    format::personal_archive::SidecarPayload payload;
};

struct ArchiveIdentity final {
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::array<std::byte, 16> parent_uuid{};
    format::BackupType backup_type{format::BackupType::kFull};
    std::uint32_t block_size{0};
};

struct ArchiveChainOpenRequest final {
    // Layers are base-first. Each password view only needs to remain valid during open().
    std::vector<ArchiveOpenRequest> layers;
    std::uint32_t maximum_chain_depth{128};
};

/// Authenticated Header/Manifest summary for composition-root dispatch (Explorer Shell, etc.).
/// content_kind is taken from the authenticated BackupHeader (AAD-bound with metadata decrypt).
struct AuthenticatedArchiveMetadata final {
    /// format::personal_archive::kContentKindVolumeSet | kContentKindFileSet
    std::uint8_t content_kind{format::personal_archive::kContentKindVolumeSet};
    /// format::kManifestContentKindVolumeSet | kManifestContentKindFileSet
    std::uint8_t manifest_content_kind{format::kManifestContentKindVolumeSet};
    format::BackupType backup_type{format::BackupType::kFull};
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::array<std::byte, 16> parent_uuid{};
    bool encryption_enabled{false};
};

/// Decrypt/authenticate archive metadata and return content_kind for composition-root dispatch.
/// Reader open paths re-authenticate independently and must agree with this metadata.
[[nodiscard]] base::Result<AuthenticatedArchiveMetadata>
authenticate_archive_metadata(const ArchiveOpenRequest& request);

[[nodiscard]] base::Result<ArchiveSidecar>
load_archive_sidecar(const std::filesystem::path& archive_path, std::string_view password,
                     std::uint64_t maximum_uncompressed_size = 256ULL * 1024ULL * 1024ULL);

struct PersonalArchiveWriteMetrics final {
    std::uint64_t prepare_microseconds{0};
    std::uint64_t persist_microseconds{0};
    std::uint64_t commit_microseconds{0};
    /// CPU time summed across block workers (can exceed prepare wall time).
    std::uint64_t prepare_hash_microseconds{0};
    std::uint64_t prepare_compress_microseconds{0};
    std::uint64_t write_file_microseconds{0};
    std::uint64_t write_file_bytes{0};
    std::uint64_t write_file_calls{0};
};

class PersonalArchiveSession final : public ports::IBackupSession {
  public:
    ~PersonalArchiveSession() override;
    PersonalArchiveSession(const PersonalArchiveSession&) = delete;
    PersonalArchiveSession& operator=(const PersonalArchiveSession&) = delete;
    PersonalArchiveSession(PersonalArchiveSession&&) = delete;
    PersonalArchiveSession& operator=(PersonalArchiveSession&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveSession>>
    create(const ArchiveCreateRequest& request);

    [[nodiscard]] base::Result<void> write_chunk(const ports::ChunkWriteRequest& request,
                                                 base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> commit(base::CancellationToken cancellation) override;
    [[nodiscard]] PersonalArchiveWriteMetrics write_metrics() const noexcept;
    void abort() noexcept override;

  private:
    struct Impl;
    explicit PersonalArchiveSession(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

struct FileArchiveCreateRequest final {
    std::filesystem::path destination;
    std::filesystem::path index_spool_directory;
    const format::Manifest& manifest;
    std::string_view password;
    bool encryption_enabled{true};
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    /// Direct parent Recovery Point file_uuid. Must be zero for Full; non-zero for Incremental.
    std::array<std::byte, 16> parent_uuid{};
    std::uint32_t block_size{4096};
    std::uint32_t chunk_size{4U * 1024U * 1024U};
    std::uint64_t split_size_bytes{0};
    ArchiveKdfParameters kdf_parameters;
};

/// V7 file_set Archive writer (Full and Incremental). Index entries are staged to a spool file
/// under index_spool_directory. Finalize sorts compact spool locators and streams leaf pages
/// (at most one leaf of FileEntryDesc resident); internal levels use BuiltIndexPage only.
///
/// FI4: Incremental layers write complete tip Index with content_storage=local|parent.
/// Parent streams carry only direct parent_stream_index (no local payload). Full layers reject
/// parent storage. Abort/destructor delete partial + spool; Catalog publish is composition root.
class PersonalFileArchiveSession final : public ports::IFileBackupSession {
  public:
    ~PersonalFileArchiveSession() override;
    PersonalFileArchiveSession(const PersonalFileArchiveSession&) = delete;
    PersonalFileArchiveSession& operator=(const PersonalFileArchiveSession&) = delete;
    PersonalFileArchiveSession(PersonalFileArchiveSession&&) = delete;
    PersonalFileArchiveSession& operator=(PersonalFileArchiveSession&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalFileArchiveSession>>
    create(const FileArchiveCreateRequest& request);

    [[nodiscard]] base::Result<void>
    write_entry(const contracts::FileEntryDesc& entry,
                base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<std::uint64_t>
    write_stream_chunk(const ports::FileChunkWriteRequest& request,
                       base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> finalize(base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> commit(base::CancellationToken cancellation) override;
    void abort() noexcept override;

  private:
    struct Impl;
    explicit PersonalFileArchiveSession(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

/// Stream lookup within one authenticated file_set Archive layer (FI5).
struct FileStreamOwnerView final {
    contracts::StableFileIdentity identity;
    contracts::FileEntryKind entry_kind{contracts::FileEntryKind::kFile};
    contracts::FileStreamDesc stream;
};

/// V7 file_set Recovery Point reader (IFileRecoveryPointReader) for one Archive layer.
/// O(1) open via Footer roots + bounded LRU page cache (ADR-0019 / L31). Secondary B+trees
/// (Entry ID / Stream / Chunk) and Namespace child offsets support log-time lookup without
/// O(N) locator tables. kDeferred defers root authentication until first Index access.
/// Parent content_storage streams are rejected on read_stream; use
/// PersonalFileArchiveChainReader to resolve them.
class PersonalFileArchiveReader final : public ports::IFileRecoveryPointReader {
  public:
    ~PersonalFileArchiveReader() override;
    PersonalFileArchiveReader(const PersonalFileArchiveReader&) = delete;
    PersonalFileArchiveReader& operator=(const PersonalFileArchiveReader&) = delete;
    PersonalFileArchiveReader(PersonalFileArchiveReader&&) = delete;
    PersonalFileArchiveReader& operator=(PersonalFileArchiveReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalFileArchiveReader>>
    open(const ArchiveOpenRequest& request);

    [[nodiscard]] const format::Manifest& manifest() const noexcept;
    [[nodiscard]] const ArchiveIdentity& identity() const noexcept;

    [[nodiscard]] std::string index_root_digest() const override;
    [[nodiscard]] std::uint64_t entry_count() const noexcept override;
    [[nodiscard]] std::uint64_t stream_count() const noexcept override;

    [[nodiscard]] base::Result<ports::FileEntryPage>
    list_children(std::uint64_t parent_entry_id, std::uint32_t maximum_results,
                  const std::optional<std::string>& continuation_token,
                  base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<contracts::FileEntryDesc>
    describe_entry(std::uint64_t entry_id, base::CancellationToken cancellation) override;

    /// Locates stream_index within this layer's Index (local or parent descriptor).
    [[nodiscard]] base::Result<FileStreamOwnerView>
    describe_stream_owner(std::uint32_t stream_index, base::CancellationToken cancellation) const;

    /// Local payload only; parent storage returns kInvalidArgument / conflict.
    [[nodiscard]] base::Result<std::size_t>
    read_stream(const ports::FileStreamReadRequest& request, std::span<std::byte> destination,
                base::CancellationToken cancellation) override;

    /// Visits every Index entry in left-to-right leaf order (one leaf page resident).
    /// Used by Verify so it does not probe entry_id in 1..entry_count.
    [[nodiscard]] base::Result<void> for_each_entry_in_leaf_order(
        base::CancellationToken cancellation,
        const std::function<base::Result<void>(const contracts::FileEntryDesc&)>& visitor) const;

    /// Full Entry ID uniqueness + parent-graph validation (explicit Verify only; ADR-0019).
    [[nodiscard]] base::Result<void>
    verify_index_and_parent_graph(base::CancellationToken cancellation) const;

  private:
    struct Impl;
    explicit PersonalFileArchiveReader(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] base::Result<void> ensure_index_loaded() const;

    std::unique_ptr<Impl> implementation_;
};

/// Totals from a recoverability Verify over a complete file_set chain (FI5).
struct FileChainVerifyResult final {
    std::uint64_t layer_count{0};
    std::uint64_t tip_entry_count{0};
    std::uint64_t tip_stream_count{0};
    /// Sum of logical bytes of every local stream on every layer (payload authenticated).
    std::uint64_t local_payload_bytes{0};
    /// Sum of logical bytes of every tip stream after full chain resolution.
    std::uint64_t tip_resolved_bytes{0};
};

/// V7 file_set chain reader: base-first layers, tip Index browse, recursive parent stream
/// resolution. Open fails closed on incomplete chain, fingerprint mismatch, cycles, or depth.
class PersonalFileArchiveChainReader final : public ports::IFileRecoveryPointReader {
  public:
    ~PersonalFileArchiveChainReader() override;
    PersonalFileArchiveChainReader(const PersonalFileArchiveChainReader&) = delete;
    PersonalFileArchiveChainReader& operator=(const PersonalFileArchiveChainReader&) = delete;
    PersonalFileArchiveChainReader(PersonalFileArchiveChainReader&&) = delete;
    PersonalFileArchiveChainReader& operator=(PersonalFileArchiveChainReader&&) = delete;

    /// Layers are base-first (Full root … tip). Passwords need only remain valid during open().
    [[nodiscard]] static base::Result<std::unique_ptr<PersonalFileArchiveChainReader>>
    open(const ArchiveChainOpenRequest& request);

    [[nodiscard]] std::size_t layer_count() const noexcept;
    [[nodiscard]] const PersonalFileArchiveReader& layer_at(std::size_t index) const;
    [[nodiscard]] const format::Manifest& tip_manifest() const noexcept;
    [[nodiscard]] const ArchiveIdentity& tip_identity() const noexcept;

    /// Tip Index root digest (hex). Browse tokens bind this together with chain_generation_digest().
    [[nodiscard]] std::string index_root_digest() const override;
    /// Base-first join of every layer Index root digest with '+'. Changes if any layer regenerates.
    [[nodiscard]] std::string chain_generation_digest() const;
    [[nodiscard]] std::uint64_t entry_count() const noexcept override;
    [[nodiscard]] std::uint64_t stream_count() const noexcept override;

    [[nodiscard]] base::Result<ports::FileEntryPage>
    list_children(std::uint64_t parent_entry_id, std::uint32_t maximum_results,
                  const std::optional<std::string>& continuation_token,
                  base::CancellationToken cancellation) override;

    [[nodiscard]] base::Result<contracts::FileEntryDesc>
    describe_entry(std::uint64_t entry_id, base::CancellationToken cancellation) override;

    /// Resolves a tip stream through parent hops to its local owner without reading payload.
    /// Used by Service restore preflight so missing parents fail before target mutation.
    [[nodiscard]] base::Result<void>
    resolve_stream_reference(std::uint32_t stream_index, base::CancellationToken cancellation) const;

    [[nodiscard]] base::Result<std::size_t>
    read_stream(const ports::FileStreamReadRequest& request, std::span<std::byte> destination,
                base::CancellationToken cancellation) override;

    /// Authenticates every layer local payload and resolves every tip stream through the chain.
    [[nodiscard]] base::Result<FileChainVerifyResult>
    verify_recoverability(std::size_t memory_budget_bytes, base::CancellationToken cancellation);

  private:
    struct Impl;
    explicit PersonalFileArchiveChainReader(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

class PersonalArchiveReader final : public ports::IRecoveryPointReader {
  public:
    ~PersonalArchiveReader() override;
    PersonalArchiveReader(const PersonalArchiveReader&) = delete;
    PersonalArchiveReader& operator=(const PersonalArchiveReader&) = delete;
    PersonalArchiveReader(PersonalArchiveReader&&) = delete;
    PersonalArchiveReader& operator=(PersonalArchiveReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveReader>>
    open(const ArchiveOpenRequest& request);

    [[nodiscard]] const format::Manifest& manifest() const noexcept;
    [[nodiscard]] const ArchiveIdentity& identity() const noexcept;
    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept override;
    [[nodiscard]] base::Result<ports::ChunkDescriptor>
    describe_chunk(std::uint64_t chunk_index) const override;
    [[nodiscard]] base::Result<ports::ChunkData>
    read_chunk(std::uint64_t chunk_index, base::CancellationToken cancellation) override;

  private:
    friend class PersonalArchiveChainReader;

    struct Impl;
    explicit PersonalArchiveReader(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveReader>>
    open_with_workers(const ArchiveOpenRequest& request,
                      std::shared_ptr<detail::BlockWorkerPool> block_workers);

    std::unique_ptr<Impl> implementation_;
};

class PersonalArchiveChainReader final : public ports::IRecoveryPointReader {
  public:
    ~PersonalArchiveChainReader() override;
    PersonalArchiveChainReader(const PersonalArchiveChainReader&) = delete;
    PersonalArchiveChainReader& operator=(const PersonalArchiveChainReader&) = delete;
    PersonalArchiveChainReader(PersonalArchiveChainReader&&) = delete;
    PersonalArchiveChainReader& operator=(PersonalArchiveChainReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveChainReader>>
    open(const ArchiveChainOpenRequest& request);

    [[nodiscard]] const format::Manifest& manifest() const noexcept;
    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept override;
    [[nodiscard]] base::Result<ports::ChunkDescriptor>
    describe_chunk(std::uint64_t chunk_index) const override;
    [[nodiscard]] base::Result<ports::ChunkData>
    read_chunk(std::uint64_t chunk_index, base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit PersonalArchiveChainReader(std::unique_ptr<Impl> implementation) noexcept;

    [[nodiscard]] static base::Result<std::vector<std::unique_ptr<PersonalArchiveReader>>>
    open_layers(const ArchiveChainOpenRequest& request,
                const std::shared_ptr<detail::BlockWorkerPool>& block_workers);

    std::unique_ptr<Impl> implementation_;
};

/// Exposes one volume from a multi-volume archive as a contiguous source_index=0 stream
/// for RestorePipeline (which currently rejects multi-volume descriptors).
class PersonalArchiveVolumeReader final : public ports::IRecoveryPointReader {
  public:
    ~PersonalArchiveVolumeReader() override;
    PersonalArchiveVolumeReader(const PersonalArchiveVolumeReader&) = delete;
    PersonalArchiveVolumeReader& operator=(const PersonalArchiveVolumeReader&) = delete;
    PersonalArchiveVolumeReader(PersonalArchiveVolumeReader&&) = delete;
    PersonalArchiveVolumeReader& operator=(PersonalArchiveVolumeReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveVolumeReader>>
    open(ports::IRecoveryPointReader& inner, const format::Manifest& manifest,
         std::uint32_t volume_index);

    [[nodiscard]] std::uint64_t logical_size_bytes() const noexcept override;
    [[nodiscard]] std::uint64_t chunk_count() const noexcept override;
    [[nodiscard]] base::Result<ports::ChunkDescriptor>
    describe_chunk(std::uint64_t chunk_index) const override;
    [[nodiscard]] base::Result<ports::ChunkData>
    read_chunk(std::uint64_t chunk_index, base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit PersonalArchiveVolumeReader(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

/// Random-access view of one Manifest volume (offset 0 = volume boot sector).
/// Builds a bounded chunk locator at open; FREE/unmapped ranges zero-fill.
/// Does not depend on NTFS and does not modify the recovery-point reader.
class PersonalArchiveVolumeRandomReader final : public ports::IRandomAccessReader {
  public:
    ~PersonalArchiveVolumeRandomReader() override;
    PersonalArchiveVolumeRandomReader(const PersonalArchiveVolumeRandomReader&) = delete;
    PersonalArchiveVolumeRandomReader& operator=(const PersonalArchiveVolumeRandomReader&) = delete;
    PersonalArchiveVolumeRandomReader(PersonalArchiveVolumeRandomReader&&) = delete;
    PersonalArchiveVolumeRandomReader& operator=(PersonalArchiveVolumeRandomReader&&) = delete;

    /// inner and manifest must outlive this reader. volume_index selects Manifest.volumes.
    [[nodiscard]] static base::Result<std::unique_ptr<PersonalArchiveVolumeRandomReader>>
    open(ports::IRecoveryPointReader& inner, const format::Manifest& manifest,
         std::uint32_t volume_index);

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override;

    [[nodiscard]] std::uint32_t volume_index() const noexcept;

  private:
    struct Impl;
    explicit PersonalArchiveVolumeRandomReader(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

// Presents one source disk as a linear image for mount:
// zero-filled holes + volume extents + raw partition-table overlay.
// Does not modify the underlying recovery-point reader.
class WholeDiskByteReader final : public ports::IRandomAccessReader {
  public:
    ~WholeDiskByteReader() override;
    WholeDiskByteReader(const WholeDiskByteReader&) = delete;
    WholeDiskByteReader& operator=(const WholeDiskByteReader&) = delete;
    WholeDiskByteReader(WholeDiskByteReader&&) = delete;
    WholeDiskByteReader& operator=(WholeDiskByteReader&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<WholeDiskByteReader>>
    open(ports::IRecoveryPointReader& inner, const format::Manifest& manifest,
         std::uint32_t source_disk_number);

    [[nodiscard]] std::uint64_t size_bytes() const noexcept override;
    [[nodiscard]] base::Result<std::size_t>
    read_at(std::uint64_t offset, std::span<std::byte> destination,
            base::CancellationToken cancellation) override;

    [[nodiscard]] std::uint32_t source_disk_number() const noexcept;
    [[nodiscard]] const format::Disk& disk() const noexcept;

  private:
    struct Impl;
    explicit WholeDiskByteReader(std::unique_ptr<Impl> implementation) noexcept;

    std::unique_ptr<Impl> implementation_;
};

} // namespace aegra::adapters::personal_archive
