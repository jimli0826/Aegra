#include "aegra/apps/service/worker_job_service.h"

#include "file_recovery_chain.h"
#include "worker_job_service_detail.h"

#include "aegra/adapters/personal_archive/personal_archive.h"
#include "aegra/adapters/windows_filesystem/windows_filesystem.h"
#include "aegra/adapters/windows_system/windows_system.h"
#include "aegra/application/file_browse_service.h"
#include "aegra/application/source_inventory_query.h"
#include "aegra/apps/service/worker_supervisor.h"
#include "aegra/contracts/file_set.h"
#include "aegra/ports/clock.h"
#include "aegra/ports/control_plane.h"
#include "aegra/ports/random.h"
#include "aegra/ports/repository_storage.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

using worker_job_detail::acknowledgement;
using worker_job_detail::random_id;

constexpr std::uint64_t kFileRestorePreflightTtlMs = 30U * 60U * 1'000U;

[[nodiscard]] char hex_digit(const unsigned value) noexcept {
    return static_cast<char>(value < 10U ? '0' + value : 'a' + (value - 10U));
}

[[nodiscard]] std::string encode_relative_path_blob(
    const std::vector<contracts::EncodedName>& components) {
    std::string encoded = "v1";
    for (const auto& component : components) {
        encoded.push_back('|');
        encoded += std::to_string(static_cast<unsigned>(component.encoding));
        encoded.push_back(':');
        for (const auto byte : component.bytes) {
            const auto value = std::to_integer<unsigned>(byte);
            encoded.push_back(hex_digit((value >> 4U) & 0x0FU));
            encoded.push_back(hex_digit(value & 0x0FU));
        }
    }
    return encoded;
}

[[nodiscard]] base::Result<std::string>
encode_target_root_identity(const contracts::FileSourceRef& target) {
    if (target.volume_identity.empty() || target.volume_identity.find('|') != std::string::npos) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "file restore target identity is invalid"});
    }
    std::string identity = "f1|";
    identity.append(target.volume_identity);
    identity.push_back('|');
    identity.append(encode_relative_path_blob(target.relative_components));
    if (identity.size() > contracts::kMaximumTargetRootIdentityBytes) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "file restore target path is too deep"});
    }
    return base::Result<std::string>::success(std::move(identity));
}

[[nodiscard]] std::string make_target_source_id(const std::string_view target_root_identity) {
    // Stable short binding for SQLite target_source_id (no paths).
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : target_root_identity) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    std::string id = "filetgt.";
    for (int shift = 60; shift >= 0; shift -= 4) {
        id.push_back(hex_digit(static_cast<unsigned>((hash >> shift) & 0x0FU)));
    }
    return id;
}

[[nodiscard]] std::string make_file_restore_fingerprint(
    const std::string_view archive_key, const std::string_view file_uuid,
    const std::string_view tip_index_digest, const std::string_view chain_generation,
    const std::uint32_t chain_depth, const contracts::FileConflictPolicy conflict,
    const bool restore_security, const std::uint64_t entry_count,
    const std::uint64_t logical_size, const std::string_view target_root_identity) {
    // FI8: filec|key|uuid|tip|chain_gen|depth|conflict|sec|count|logical|target
    std::string out = "filec|";
    out.append(archive_key);
    out.push_back('|');
    out.append(file_uuid);
    out.push_back('|');
    out.append(tip_index_digest);
    out.push_back('|');
    out.append(chain_generation);
    out.push_back('|');
    out += std::to_string(chain_depth);
    out.push_back('|');
    out += std::to_string(static_cast<unsigned>(conflict));
    out.push_back('|');
    out.push_back(restore_security ? '1' : '0');
    out.push_back('|');
    out += std::to_string(entry_count);
    out.push_back('|');
    out += std::to_string(logical_size);
    out.push_back('|');
    out.append(target_root_identity);
    return out;
}

struct ParsedFileRestoreFingerprint final {
    std::string archive_key;
    std::string file_uuid;
    std::string tip_index_digest;
    std::string chain_generation;
    std::uint32_t chain_depth{1};
    contracts::FileConflictPolicy conflict_policy{contracts::FileConflictPolicy::kFail};
    bool restore_security{true};
    std::uint64_t entry_count{0};
    std::uint64_t logical_size_bytes{0};
    std::string target_root_identity;
};

[[nodiscard]] base::Result<ParsedFileRestoreFingerprint>
parse_file_restore_fingerprint(const std::string_view fingerprint) {
    if (!fingerprint.starts_with("filec|")) {
        return base::Result<ParsedFileRestoreFingerprint>::failure(
            {base::ErrorCode::kConflict, "restore preflight is not a file restore"});
    }
    // filec|key|uuid|tip|chain_gen|depth|conflict|sec|count|logical|target...
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    while (start <= fingerprint.size()) {
        const auto bar = fingerprint.find('|', start);
        if (bar == std::string_view::npos) {
            parts.push_back(fingerprint.substr(start));
            break;
        }
        parts.push_back(fingerprint.substr(start, bar - start));
        start = bar + 1;
    }
    if (parts.size() < 11 || parts[0] != "filec") {
        return base::Result<ParsedFileRestoreFingerprint>::failure(
            {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
    }
    ParsedFileRestoreFingerprint parsed;
    parsed.archive_key = std::string(parts[1]);
    parsed.file_uuid = std::string(parts[2]);
    parsed.tip_index_digest = std::string(parts[3]);
    parsed.chain_generation = std::string(parts[4]);
    if (parsed.archive_key.empty() || parsed.file_uuid.empty() ||
        parsed.tip_index_digest.empty() || parsed.chain_generation.empty()) {
        return base::Result<ParsedFileRestoreFingerprint>::failure(
            {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
    }
    {
        unsigned depth = 0;
        const auto* begin = parts[5].data();
        const auto* end = begin + parts[5].size();
        if (std::from_chars(begin, end, depth).ec != std::errc{} || depth == 0 ||
            depth > contracts::kMaximumFileChainDepth) {
            return base::Result<ParsedFileRestoreFingerprint>::failure(
                {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
        }
        parsed.chain_depth = static_cast<std::uint32_t>(depth);
    }
    {
        unsigned conflict = 0;
        const auto* begin = parts[6].data();
        const auto* end = begin + parts[6].size();
        if (std::from_chars(begin, end, conflict).ec != std::errc{} ||
            !contracts::is_known_file_conflict_policy(
                static_cast<contracts::FileConflictPolicy>(conflict))) {
            return base::Result<ParsedFileRestoreFingerprint>::failure(
                {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
        }
        parsed.conflict_policy = static_cast<contracts::FileConflictPolicy>(conflict);
    }
    if (parts[7].size() != 1 || (parts[7][0] != '0' && parts[7][0] != '1')) {
        return base::Result<ParsedFileRestoreFingerprint>::failure(
            {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
    }
    parsed.restore_security = parts[7][0] == '1';
    {
        const auto* begin = parts[8].data();
        const auto* end = begin + parts[8].size();
        if (std::from_chars(begin, end, parsed.entry_count).ec != std::errc{} ||
            parsed.entry_count == 0) {
            return base::Result<ParsedFileRestoreFingerprint>::failure(
                {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
        }
    }
    {
        const auto* begin = parts[9].data();
        const auto* end = begin + parts[9].size();
        if (std::from_chars(begin, end, parsed.logical_size_bytes).ec != std::errc{}) {
            return base::Result<ParsedFileRestoreFingerprint>::failure(
                {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
        }
    }
    // Target identity may contain '|' (path blob). Rejoin remaining parts.
    std::string target;
    for (std::size_t index = 10; index < parts.size(); ++index) {
        if (index > 10) {
            target.push_back('|');
        }
        target.append(parts[index]);
    }
    if (target.empty() || target.size() > contracts::kMaximumTargetRootIdentityBytes) {
        return base::Result<ParsedFileRestoreFingerprint>::failure(
            {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
    }
    parsed.target_root_identity = std::move(target);
    return base::Result<ParsedFileRestoreFingerprint>::success(std::move(parsed));
}

[[nodiscard]] base::Result<std::uint64_t> parse_entry_id(const std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    if (std::from_chars(begin, end, value).ec != std::errc{} || value == 0) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    return base::Result<std::uint64_t>::success(value);
}

struct SelectionClosureTotals final {
    std::uint64_t entry_count{0};
    std::uint64_t logical_size_bytes{0};
    std::uint64_t maximum_file_size_bytes{0};
};

[[nodiscard]] base::Result<void>
resolve_entry_streams(adapters::personal_archive::PersonalFileArchiveChainReader& chain,
                      const contracts::FileEntryDesc& entry,
                      const base::CancellationToken cancellation) {
    if (entry.kind != contracts::FileEntryKind::kFile) {
        return base::Result<void>::success();
    }
    for (const auto& stream : entry.streams) {
        auto resolved = chain.resolve_stream_reference(stream.stream_index, cancellation);
        if (!resolved) {
            return base::Result<void>::failure(
                map_file_recover_open_error(resolved.error(), true));
        }
    }
    return base::Result<void>::success();
}

/// Selection closure for free-space preflight + parent-stream resolution (FI8).
/// Directory seeds expand to all reachable descendants (tip logical bytes sum).
[[nodiscard]] base::Result<SelectionClosureTotals>
compute_selection_closure_totals(
    adapters::personal_archive::PersonalFileArchiveChainReader& chain,
    const std::vector<std::string>& entry_ids, const base::CancellationToken cancellation) {
    SelectionClosureTotals totals;
    std::unordered_set<std::uint64_t> seen;
    std::vector<std::uint64_t> directory_queue;
    for (const auto& entry_text : entry_ids) {
        auto entry_id = parse_entry_id(entry_text);
        if (!entry_id) {
            return base::Result<SelectionClosureTotals>::failure(entry_id.error());
        }
        if (!seen.insert(entry_id.value()).second) {
            continue;
        }
        auto described = chain.describe_entry(entry_id.value(), cancellation);
        if (!described) {
            return base::Result<SelectionClosureTotals>::failure(described.error());
        }
        if (auto streams = resolve_entry_streams(chain, described.value(), cancellation);
            !streams) {
            return base::Result<SelectionClosureTotals>::failure(streams.error());
        }
        if (totals.logical_size_bytes >
            (std::numeric_limits<std::uint64_t>::max)() - described.value().logical_size) {
            return base::Result<SelectionClosureTotals>::failure(
                {base::ErrorCode::kInvalidArgument, "file restore logical size overflow"});
        }
        totals.logical_size_bytes += described.value().logical_size;
        if (described.value().kind == contracts::FileEntryKind::kFile) {
            totals.maximum_file_size_bytes =
                (std::max)(totals.maximum_file_size_bytes, described.value().logical_size);
        }
        ++totals.entry_count;
        if (described.value().kind == contracts::FileEntryKind::kDirectory) {
            directory_queue.push_back(entry_id.value());
        }
    }
    while (!directory_queue.empty()) {
        if (cancellation.stop_requested()) {
            return base::Result<SelectionClosureTotals>::failure(
                {base::ErrorCode::kCancelled, "file restore preflight cancelled"});
        }
        const auto parent = directory_queue.back();
        directory_queue.pop_back();
        std::optional<std::string> token;
        do {
            auto page = chain.list_children(parent, 256, token, cancellation);
            if (!page) {
                return base::Result<SelectionClosureTotals>::failure(page.error());
            }
            for (const auto& summary : page.value().items) {
                auto child_id = parse_entry_id(summary.entry_id);
                if (!child_id) {
                    return base::Result<SelectionClosureTotals>::failure(child_id.error());
                }
                if (!seen.insert(child_id.value()).second) {
                    continue;
                }
                auto described = chain.describe_entry(child_id.value(), cancellation);
                if (!described) {
                    return base::Result<SelectionClosureTotals>::failure(described.error());
                }
                if (auto streams = resolve_entry_streams(chain, described.value(), cancellation);
                    !streams) {
                    return base::Result<SelectionClosureTotals>::failure(streams.error());
                }
                if (totals.logical_size_bytes >
                    (std::numeric_limits<std::uint64_t>::max)() - described.value().logical_size) {
                    return base::Result<SelectionClosureTotals>::failure(
                        {base::ErrorCode::kInvalidArgument, "file restore logical size overflow"});
                }
                totals.logical_size_bytes += described.value().logical_size;
                if (described.value().kind == contracts::FileEntryKind::kFile) {
                    totals.maximum_file_size_bytes =
                        (std::max)(totals.maximum_file_size_bytes,
                                   described.value().logical_size);
                }
                ++totals.entry_count;
                if (described.value().kind == contracts::FileEntryKind::kDirectory) {
                    directory_queue.push_back(child_id.value());
                }
            }
            token = page.value().continuation_token;
        } while (token.has_value());
    }
    return base::Result<SelectionClosureTotals>::success(std::move(totals));
}

[[nodiscard]] base::Result<std::vector<std::uint16_t>>
utf8_to_utf16_units(const std::string& utf8) {
    if (utf8.empty()) {
        return base::Result<std::vector<std::uint16_t>>::failure(
            {base::ErrorCode::kInvalidArgument, "path is empty"});
    }
    std::u8string encoded;
    encoded.reserve(utf8.size());
    for (const char item : utf8) {
        encoded.push_back(static_cast<char8_t>(item));
    }
    std::filesystem::path path(encoded);
    const auto& native = path.native();
    std::vector<std::uint16_t> units;
    units.reserve(native.size());
    for (const wchar_t character : native) {
        units.push_back(static_cast<std::uint16_t>(character));
    }
    if (units.empty()) {
        return base::Result<std::vector<std::uint16_t>>::failure(
            {base::ErrorCode::kInvalidArgument, "path is empty"});
    }
    return base::Result<std::vector<std::uint16_t>>::success(std::move(units));
}

[[nodiscard]] base::Result<ports::FileSinkCapabilities>
probe_target_capabilities(const contracts::FileSourceRef& target,
                          const base::CancellationToken cancellation) {
    auto root = utf8_to_utf16_units(target.volume_identity);
    if (!root) {
        return base::Result<ports::FileSinkCapabilities>::failure(root.error());
    }
    // Join volume root + relative components into sink root.
    // Volume GUID roots must keep a trailing '\\' (GetDiskFreeSpaceExW / CreateFile).
    std::wstring path(root.value().begin(), root.value().end());
    while (!path.empty() && (path.back() == L'\\' || path.back() == L'/')) {
        path.pop_back();
    }
    if (target.relative_components.empty()) {
        path.push_back(L'\\');
    } else {
        for (const auto& component : target.relative_components) {
            if (component.bytes.empty() || (component.bytes.size() % 2U) != 0U) {
                return base::Result<ports::FileSinkCapabilities>::failure(
                    {base::ErrorCode::kInvalidArgument, "file restore target path is invalid"});
            }
            std::wstring piece(component.bytes.size() / 2U, L'\0');
            std::memcpy(piece.data(), component.bytes.data(), component.bytes.size());
            path.push_back(L'\\');
            path.append(piece);
        }
    }
    adapters::windows_filesystem::WindowsFileTreeSinkOpenRequest open_request;
    open_request.target_root_utf16.assign(path.begin(), path.end());
    auto sink = adapters::windows_filesystem::WindowsFileTreeSink::open(open_request);
    if (!sink) {
        return base::Result<ports::FileSinkCapabilities>::failure(sink.error());
    }
    auto caps = sink.value()->capabilities(cancellation);
    if (!caps) {
        return base::Result<ports::FileSinkCapabilities>::failure(caps.error());
    }
    return caps;
}

[[nodiscard]] std::string file_restore_request_fingerprint(const std::string_view preflight_token) {
    return "start-file-restore|" + std::string(preflight_token);
}

[[nodiscard]] base::Result<std::string>
source_id_from_volume_identity(const std::string_view identity) {
    const auto begin = identity.find('{');
    const auto end = identity.find('}', begin == std::string_view::npos ? 0 : begin + 1);
    if (begin == std::string_view::npos || end == std::string_view::npos || end <= begin + 1) {
        return base::Result<std::string>::failure(
            {base::ErrorCode::kInvalidArgument, "file_source.volume_identity_mismatch"});
    }
    std::string id = "vol.";
    id.append(identity.substr(begin + 1, end - begin - 1));
    std::ranges::transform(id, id.begin(), [](const unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return base::Result<std::string>::success(std::move(id));
}

} // namespace

base::Result<contracts::FileRestorePreflight>
WorkerJobService::prepare_file_restore(const contracts::PrepareFileRestoreRequest& request,
                                       const ports::FileBrowseCaller& caller,
                                       const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_prepare_file_restore_request(request); !valid) {
        return base::Result<contracts::FileRestorePreflight>::failure(valid.error());
    }
    if (file_browse_ == nullptr) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kConflict, "file.browse is unavailable"});
    }
    if (!request.repository_connection_id || request.repository_connection_id->empty()) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "repository_connection_id is required"});
    }
    auto target_ref =
        file_browse_->resolve_selection(caller, request.target_node_token,
                                        contracts::FileRecursion::kSelfOnly, {}, cancellation);
    if (!target_ref) {
        return base::Result<contracts::FileRestorePreflight>::failure(target_ref.error());
    }
    if (target_ref.value().entry_kind != contracts::FileEntryKind::kDirectory) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "file_restore.target_not_directory"});
    }
    auto volume_source_id = source_id_from_volume_identity(target_ref.value().volume_identity);
    if (!volume_source_id) {
        return base::Result<contracts::FileRestorePreflight>::failure(volume_source_id.error());
    }
    auto inventory = source_inventory_.resolve_source(volume_source_id.value(), cancellation);
    if (!inventory) {
        return base::Result<contracts::FileRestorePreflight>::failure(inventory.error());
    }
    // Volume inventory.is_system means "hosts Windows" (volume restore PE gate). File restore
    // writes into a chosen directory tree and must allow user folders on that volume
    // (Documents, Desktop, Downloads, …). Do not reject the whole system volume here.
    if (inventory.value().availability != contracts::SourceAvailability::kAvailable ||
        inventory.value().is_read_only) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kConflict, "file restore target is unavailable"});
    }
    auto target_identity = encode_target_root_identity(target_ref.value());
    if (!target_identity) {
        return base::Result<contracts::FileRestorePreflight>::failure(target_identity.error());
    }
    auto target_capabilities = probe_target_capabilities(target_ref.value(), cancellation);
    if (!target_capabilities) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            target_capabilities.error());
    }
    if (request.restore_security &&
        !target_capabilities.value().supports_security_descriptor) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "file_restore.target_capability_missing"});
    }
    // Prefer volume inventory free space (enumerator path is reliable). Sink probe can
    // under-report 0 when the path form confuses GetDiskFreeSpaceExW; take the larger
    // known free value so a false 0 does not block restores on disks with free capacity.
    const auto inventory_free = inventory.value().free_bytes > inventory.value().capacity_bytes
                                    ? inventory.value().capacity_bytes
                                    : inventory.value().free_bytes;
    const auto free_for_capacity =
        (std::max)(target_capabilities.value().free_bytes, inventory_free);
    auto chain = open_file_recovery_chain(control_plane_, storage_factory_,
                                          *request.repository_connection_id,
                                          request.recovery_point_id, request.archive_secret_ref,
                                          cancellation);
    if (!chain) {
        return base::Result<contracts::FileRestorePreflight>::failure(chain.error());
    }
    const auto& tip = chain.value().catalog_layers.back();
    auto closure =
        compute_selection_closure_totals(*chain.value().reader, request.entry_ids, cancellation);
    if (!closure) {
        return base::Result<contracts::FileRestorePreflight>::failure(closure.error());
    }
    if (closure.value().maximum_file_size_bytes >
        target_capabilities.value().maximum_file_size_bytes) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "file_restore.target_file_too_large"});
    }
    const auto logical_size = closure.value().logical_size_bytes;
    if (free_for_capacity < logical_size) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInsufficientSpace, "file_restore.target_full"});
    }
    const auto now = clock_.now_utc_ms();
    if (now < 0) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInternal, "file restore preflight clock is invalid"});
    }
    const auto now_u = static_cast<std::uint64_t>(now);
    if (now_u > static_cast<std::uint64_t>((std::numeric_limits<std::int64_t>::max)()) -
                     kFileRestorePreflightTtlMs) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInternal, "file restore preflight clock is invalid"});
    }
    auto token = random_id("preflight-", random_, cancellation);
    if (!token) {
        return base::Result<contracts::FileRestorePreflight>::failure(token.error());
    }
    ports::RestorePreflightRecord record;
    record.preflight_token = token.value();
    record.repository_connection_id = *request.repository_connection_id;
    record.repository_uuid = tip.repository_uuid;
    record.recovery_point_id = request.recovery_point_id;
    record.target_source_id = make_target_source_id(target_identity.value());
    // Fingerprint binds seed entry_ids count (wire selection), not expanded closure size.
    record.chain_fingerprint = make_file_restore_fingerprint(
        tip.archive_main_key, tip.file_uuid, chain.value().tip_index_digest,
        chain.value().chain_generation_digest,
        static_cast<std::uint32_t>(chain.value().catalog_layers.size()), request.conflict_policy,
        request.restore_security, request.entry_ids.size(), logical_size, target_identity.value());
    if (record.chain_fingerprint.size() > 4'096) {
        return base::Result<contracts::FileRestorePreflight>::failure(
            {base::ErrorCode::kInvalidArgument, "file restore preflight fingerprint is too large"});
    }
    record.logical_size_bytes = logical_size;
    record.target_capacity_bytes = free_for_capacity;
    record.chain_depth = static_cast<std::uint32_t>(chain.value().catalog_layers.size());
    record.created_utc_ms = now_u;
    record.expires_utc_ms = now_u + kFileRestorePreflightTtlMs;
    record.entry_ids = request.entry_ids;
    auto unit = control_plane_.begin_unit_of_work(cancellation);
    if (!unit) {
        return base::Result<contracts::FileRestorePreflight>::failure(unit.error());
    }
    if (auto inserted = unit.value()->restore_preflights().insert(record, cancellation);
        !inserted) {
        unit.value()->rollback();
        return base::Result<contracts::FileRestorePreflight>::failure(inserted.error());
    }
    if (auto committed = unit.value()->commit(cancellation); !committed) {
        return base::Result<contracts::FileRestorePreflight>::failure(committed.error());
    }
    contracts::FileRestorePreflight preflight;
    preflight.preflight_token = record.preflight_token;
    preflight.repository_connection_id = record.repository_connection_id;
    preflight.recovery_point_id = record.recovery_point_id;
    // Closure size (after directory expansion), not raw seed count.
    preflight.entry_count = closure.value().entry_count;
    preflight.logical_size_bytes = record.logical_size_bytes;
    preflight.target_free_bytes = record.target_capacity_bytes;
    preflight.conflict_policy = request.conflict_policy;
    preflight.expires_utc_ms = record.expires_utc_ms;
    preflight.restore_eligible = true;
    preflight.message_code = "file_restore.preflight_ok";
    return base::Result<contracts::FileRestorePreflight>::success(std::move(preflight));
}

base::Result<contracts::CommandAcknowledgement>
WorkerJobService::start_file_restore(const contracts::StartFileRestoreCommand& command,
                                     const std::string_view idempotency_key,
                                     const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_start_file_restore_command(command); !valid) {
        return base::Result<contracts::CommandAcknowledgement>::failure(valid.error());
    }
    if (idempotency_key.empty()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kInvalidArgument, "idempotency key is required"});
    }
    auto existing = control_plane_.get_job_by_idempotency_key(idempotency_key, cancellation);
    if (!existing) {
        return base::Result<contracts::CommandAcknowledgement>::failure(existing.error());
    }
    const auto fingerprint = file_restore_request_fingerprint(command.preflight_token);
    if (existing.value()) {
        if (existing.value()->operation != contracts::JobOperation::kRestore ||
            existing.value()->content_kind != contracts::ContentKind::kFileSet ||
            existing.value()->request_fingerprint != fingerprint) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                {base::ErrorCode::kConflict, "idempotency key request mismatch"});
        }
        return base::Result<contracts::CommandAcknowledgement>::success(
            acknowledgement(existing.value()->job_id, contracts::CommandDisposition::kReplayed,
                            existing.value()->job_id));
    }
    auto preflight = control_plane_.get_restore_preflight(command.preflight_token, cancellation);
    if (!preflight) {
        return base::Result<contracts::CommandAcknowledgement>::failure(preflight.error());
    }
    if (!preflight.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kNotFound, "file restore preflight was not found"});
    }
    const auto& record = *preflight.value();
    if (!record.chain_fingerprint.starts_with("filec|")) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "volume PrepareRestore token cannot start file restore"});
    }
    const auto now = static_cast<std::uint64_t>((std::max)(clock_.now_utc_ms(), 0LL));
    if (now >= record.expires_utc_ms) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "file_restore.preflight_expired"});
    }
    auto parsed = parse_file_restore_fingerprint(record.chain_fingerprint);
    if (!parsed) {
        return base::Result<contracts::CommandAcknowledgement>::failure(parsed.error());
    }
    if (parsed.value().file_uuid != record.recovery_point_id ||
        parsed.value().entry_count != record.entry_ids.size() ||
        parsed.value().chain_depth != record.chain_depth) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "file restore preflight fingerprint is corrupt"});
    }
    auto by_token = control_plane_.get_job_by_preflight_token(command.preflight_token, cancellation);
    if (!by_token) {
        return base::Result<contracts::CommandAcknowledgement>::failure(by_token.error());
    }
    if (by_token.value()) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "file_restore.preflight_consumed"});
    }
    // Re-open and revalidate full chain generation before Start (any layer change rejects).
    auto chain = open_file_recovery_chain(control_plane_, storage_factory_,
                                          record.repository_connection_id,
                                          record.recovery_point_id, command.archive_secret_ref,
                                          cancellation);
    if (!chain) {
        return base::Result<contracts::CommandAcknowledgement>::failure(chain.error());
    }
    const auto& tip = chain.value().catalog_layers.back();
    if (tip.archive_main_key != parsed.value().archive_key || tip.file_uuid != parsed.value().file_uuid ||
        chain.value().catalog_layers.size() != parsed.value().chain_depth ||
        chain.value().tip_index_digest != parsed.value().tip_index_digest ||
        chain.value().chain_generation_digest != parsed.value().chain_generation) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            {base::ErrorCode::kConflict, "file restore chain changed after preflight"});
    }
    auto job_id = random_id("job-", random_, cancellation);
    auto trace_id = random_id("trace-", random_, cancellation);
    if (!job_id || !trace_id) {
        return base::Result<contracts::CommandAcknowledgement>::failure(
            !job_id ? job_id.error() : trace_id.error());
    }
    contracts::JobRequest worker;
    worker.job_id = job_id.value();
    worker.tenant_id = "personal";
    worker.operation = contracts::JobOperation::kRestore;
    worker.content_kind = contracts::ContentKind::kFileSet;
    worker.source_refs = chain.value().archive_paths_utf8;
    worker.target_ref.clear();
    if (chain.value().password.empty()) {
        worker.credential_refs.assign(worker.source_refs.size(), contracts::SecretRef{});
    } else {
        auto protected_secret = adapters::windows_system::protect_local_machine_secret(
            chain.value().password, job_id.value());
        if (!protected_secret) {
            return base::Result<contracts::CommandAcknowledgement>::failure(
                protected_secret.error());
        }
        worker.credential_refs.reserve(worker.source_refs.size());
        for (std::size_t index = 0; index < worker.source_refs.size(); ++index) {
            worker.credential_refs.push_back(protected_secret.value());
        }
    }
    worker.trace_id = trace_id.value();
    contracts::FileRestoreTarget restore_target;
    restore_target.target_root_identity = parsed.value().target_root_identity;
    restore_target.entry_ids = record.entry_ids;
    restore_target.conflict_policy = parsed.value().conflict_policy;
    restore_target.restore_security = parsed.value().restore_security;
    worker.file_restore_target = std::move(restore_target);

    WorkerJobRequest request;
    request.worker_request = std::move(worker);
    request.source_ids = {record.recovery_point_id};
    request.repository_connection_id = record.repository_connection_id;
    request.idempotency_key = std::string(idempotency_key);
    request.request_fingerprint = fingerprint;
    request.preflight_token = command.preflight_token;
    request.target_source_id = record.target_source_id;
    auto submitted = supervisor_.submit(request, cancellation);
    if (!submitted) {
        return base::Result<contracts::CommandAcknowledgement>::failure(submitted.error());
    }
    return base::Result<contracts::CommandAcknowledgement>::success(
        acknowledgement(job_id.value(), contracts::CommandDisposition::kAccepted, job_id.value()));
}

} // namespace aegra::apps::service
