#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive_sidecar.h"
#include "aegra/ports/backup_session.h"
#include "aegra/ports/file_backup_session.h"
#include "aegra/ports/file_recovery_point.h"
#include "aegra/ports/random_access.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>
#include <vector>

namespace aegra::adapters::personal_archive {

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
};

struct ArchiveOpenRequest final {
    std::filesystem::path source;
    std::string_view password;
    std::uint64_t maximum_metadata_size{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_payload_size{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_logical_size{1024ULL * 1024ULL * 1024ULL};
    std::uint32_t maximum_split_parts{10'000};
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

[[nodiscard]] base::Result<ArchiveSidecar>
load_archive_sidecar(const std::filesystem::path& archive_path, std::string_view password,
                     std::uint64_t maximum_uncompressed_size = 256ULL * 1024ULL * 1024ULL);

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
    std::uint32_t block_size{4096};
    std::uint32_t chunk_size{4U * 1024U * 1024U};
    std::uint64_t split_size_bytes{0};
    ArchiveKdfParameters kdf_parameters;
};

/// V7 file_set Archive writer. Index entries are staged to a spool file under
/// index_spool_directory; the full entry set is not retained as a single in-memory tree
/// beyond finalize page assembly (bounded leaf pages).
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
    [[nodiscard]] base::Result<void>
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

/// V7 file_set Recovery Point reader (IFileRecoveryPointReader).
/// Loads single-leaf roots and one-level multi-leaf trees (leaves + internal root).
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

    [[nodiscard]] base::Result<std::size_t>
    read_stream(const ports::FileStreamReadRequest& request, std::span<std::byte> destination,
                base::CancellationToken cancellation) override;

  private:
    struct Impl;
    explicit PersonalFileArchiveReader(std::unique_ptr<Impl> implementation) noexcept;

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
    struct Impl;
    explicit PersonalArchiveReader(std::unique_ptr<Impl> implementation) noexcept;

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
