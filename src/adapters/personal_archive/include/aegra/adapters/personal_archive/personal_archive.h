#pragma once

#include "aegra/base/result.h"
#include "aegra/format/manifest.h"
#include "aegra/format/personal_archive_sidecar.h"
#include "aegra/ports/backup_session.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

namespace aegra::adapters::personal_archive {

struct ArchiveKdfParameters final {
    std::uint64_t opslimit{3};
    std::uint64_t memlimit_bytes{256ULL * 1024ULL * 1024ULL};
};

struct ArchiveCreateRequest final {
    std::filesystem::path destination;
    const format::Manifest& manifest;
    std::string_view password;
    std::array<std::byte, 16> file_uuid{};
    std::array<std::byte, 16> backup_set_uuid{};
    std::uint32_t block_size{0};
    std::uint32_t chunk_size{0};
    std::uint32_t source_index{0};
    ArchiveKdfParameters kdf_parameters;
};

struct ArchiveOpenRequest final {
    std::filesystem::path source;
    std::string_view password;
    std::uint64_t maximum_metadata_size{64ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_payload_size{1024ULL * 1024ULL * 1024ULL};
    std::uint64_t maximum_chunk_logical_size{1024ULL * 1024ULL * 1024ULL};
};

struct ArchiveSidecar final {
    std::uint32_t block_size{0};
    std::array<std::byte, 16> file_uuid{};
    format::personal_archive::SidecarPayload payload;
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

} // namespace aegra::adapters::personal_archive
