#pragma once

#include "personal_archive_preamble.h"

#include "aegra/adapters/crypto_sodium/payload_crypto.h"
#include "aegra/base/result.h"
#include "aegra/contracts/file_set.h"
#include "aegra/format/file_index.h"
#include "aegra/format/personal_archive.h"
#include "aegra/ports/file_recovery_point.h"

#include <array>
#include <cstdint>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace aegra::adapters::personal_archive::lazy_index {

inline constexpr std::size_t kPageCacheCapacity = 32;

struct DecodedPage final {
    format::personal_archive::FileIndexPageHeader header;
    std::vector<std::byte> plaintext;
    std::uint64_t offset{0};
};

struct PageRef final {
    std::uint64_t page_id{0};
    std::uint64_t offset{0};
};

struct StreamChunkLocator final {
    format::personal_archive::FileStreamChunkHeader header;
    std::vector<format::personal_archive::BlockEntry> entries;
    std::uint64_t payload_offset{0};
};

/// Bounded LRU of authenticated index pages (ADR-0019 / L31).
struct PageCache final {
    struct Slot final {
        std::uint64_t offset{0};
        std::uint64_t page_id{0};
        format::personal_archive::FileIndexPageHeader header{};
        std::vector<std::byte> plaintext;
        std::uint32_t tick{0};
        bool occupied{false};
    };

    std::array<Slot, kPageCacheCapacity> slots{};
    std::uint32_t clock{1};
};

struct OpenedFileArchive final {
    detail::ParsedPreamble preamble;
    format::personal_archive::EncodedBackupHeader part_header{};
    format::personal_archive::BackupFooter footer;
    std::unique_ptr<crypto_sodium::PayloadCipher> payload_cipher;
    std::unique_ptr<crypto_sodium::PayloadCipher> index_cipher;
    std::filesystem::path path;
    bool encryption_enabled{false};
    /// Roots authenticated (eager open or after deferred ensure).
    bool roots_ready{false};
    mutable PageCache page_cache;
    /// One decoded namespace leaf for hot describe/list paths.
    mutable std::uint64_t leaf_cache_offset{0};
    mutable std::vector<contracts::FileEntryDesc> leaf_cache_entries;
};

[[nodiscard]] base::Result<std::vector<std::byte>>
read_exact(std::ifstream& input, std::uint64_t offset, std::size_t size);

[[nodiscard]] base::Result<std::uint64_t> read_stream_size(std::ifstream& input);

[[nodiscard]] std::string digest_to_hex(const std::array<std::byte, 32>& digest);

[[nodiscard]] base::Result<std::vector<std::byte>>
make_file_chunk_aad(const format::personal_archive::EncodedBackupHeader& part_header,
                    std::uint64_t body_size,
                    const format::personal_archive::FileStreamChunkHeader& header,
                    std::span<const format::personal_archive::BlockEntry> entries);

/// Authenticate Footer roots only (O(1) open). Does not scan leaves or chunks.
[[nodiscard]] base::Result<void> prepare_roots(std::ifstream& input, OpenedFileArchive& state);

[[nodiscard]] base::Result<void>
ensure_roots(std::ifstream& input, OpenedFileArchive& state);

[[nodiscard]] base::Result<contracts::FileEntryDesc>
load_entry_by_id(std::ifstream& input, OpenedFileArchive& state, std::uint64_t entry_id);

[[nodiscard]] base::Result<format::file_index::StreamIndexRecord>
lookup_stream_record(std::ifstream& input, OpenedFileArchive& state, std::uint32_t stream_index);

[[nodiscard]] base::Result<StreamChunkLocator>
load_chunk_locator(std::ifstream& input, OpenedFileArchive& state, std::uint64_t chunk_index);

[[nodiscard]] base::Result<ports::FileEntryPage>
list_children(std::ifstream& input, OpenedFileArchive& state, std::uint64_t parent_entry_id,
              std::uint32_t maximum_results, std::uint64_t start_matched,
              const base::CancellationToken& cancellation);

[[nodiscard]] base::Result<void> for_each_entry_in_leaf_order(
    std::ifstream& input, OpenedFileArchive& state, const base::CancellationToken& cancellation,
    const std::function<base::Result<void>(const contracts::FileEntryDesc&)>& visitor);

/// Full parent-graph / unique-id validation for explicit Verify (O(N log N) via Entry ID index).
[[nodiscard]] base::Result<void>
verify_entry_id_index_and_parent_graph(std::ifstream& input, OpenedFileArchive& state,
                                       const base::CancellationToken& cancellation);

[[nodiscard]] base::Result<OpenedFileArchive>
open_file_archive_state(std::ifstream& input, const ArchiveOpenRequest& request,
                        std::uint64_t file_size);

[[nodiscard]] base::Result<std::uint64_t> parse_token(const std::optional<std::string>& token);

} // namespace aegra::adapters::personal_archive::lazy_index
