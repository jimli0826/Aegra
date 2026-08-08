#include "file_recovery_point_query.h"

#include "file_recovery_chain.h"

#include "aegra/contracts/file_set.h"
#include "aegra/ports/file_recovery_point.h"

#include <Windows.h>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kMessageTokenInvalid = "file_recover.token_invalid";
constexpr std::size_t kMaximumDisplayNameBytes = 256;

[[nodiscard]] base::Result<std::uint64_t> parse_u64_text(const std::string_view text) {
    if (text.empty() || text.size() > 20) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    std::uint64_t value = 0;
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end) {
        return base::Result<std::uint64_t>::failure(
            {base::ErrorCode::kInvalidArgument, "entry id is invalid"});
    }
    return base::Result<std::uint64_t>::success(value);
}

/// UTF-16LE EncodedName → UTF-8 display label (Chinese and other non-ASCII names).
[[nodiscard]] std::string project_display_name(const contracts::EncodedName& name) {
    if (name.bytes.empty() || (name.bytes.size() % 2U) != 0U) {
        return ".";
    }
    const auto unit_count = name.bytes.size() / 2U;
    std::wstring wide(unit_count, L'\0');
    std::memcpy(wide.data(), name.bytes.data(), name.bytes.size());
    const int required =
        ::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0,
                              nullptr, nullptr);
    if (required <= 0) {
        return ".";
    }
    std::string utf8(static_cast<std::size_t>(required), '\0');
    if (::WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(),
                              required, nullptr, nullptr) != required) {
        return ".";
    }
    std::string projected;
    projected.reserve(utf8.size());
    for (const unsigned char unit : utf8) {
        if (unit >= 0x20U && unit != 0x7FU) {
            projected.push_back(static_cast<char>(unit));
        }
    }
    if (projected.empty()) {
        return ".";
    }
    if (projected.size() > kMaximumDisplayNameBytes) {
        auto limit = kMaximumDisplayNameBytes;
        while (limit > 0U && limit < projected.size() &&
               (static_cast<unsigned char>(projected[limit]) & 0xC0U) == 0x80U) {
            --limit;
        }
        projected.resize(limit);
        if (projected.empty()) {
            return ".";
        }
    }
    return projected;
}

struct BoundContinuation final {
    std::string chain_generation;
    std::string tip_index_generation;
    std::string parent_entry_id;
    std::string reader_token;
};

[[nodiscard]] base::Result<std::optional<std::string>>
encode_continuation(const std::string& chain_generation, const std::string& tip_index_generation,
                    const std::string& parent_entry_id,
                    const std::optional<std::string>& reader_token) {
    if (!reader_token) {
        return base::Result<std::optional<std::string>>::success(std::nullopt);
    }
    if (chain_generation.empty() || tip_index_generation.empty() || parent_entry_id.empty() ||
        reader_token->empty() || chain_generation.find('|') != std::string::npos ||
        tip_index_generation.find('|') != std::string::npos ||
        parent_entry_id.find('|') != std::string::npos ||
        reader_token->find('|') != std::string::npos) {
        return base::Result<std::optional<std::string>>::failure(
            {base::ErrorCode::kInternal, "continuation binding is invalid"});
    }
    std::string token;
    token.reserve(chain_generation.size() + tip_index_generation.size() + parent_entry_id.size() +
                  reader_token->size() + 3U);
    token.append(chain_generation);
    token.push_back('|');
    token.append(tip_index_generation);
    token.push_back('|');
    token.append(parent_entry_id);
    token.push_back('|');
    token.append(*reader_token);
    return base::Result<std::optional<std::string>>::success(std::move(token));
}

[[nodiscard]] base::Result<BoundContinuation>
decode_continuation(const std::optional<std::string>& token, const std::string& expected_parent) {
    if (!token) {
        return base::Result<BoundContinuation>::success(BoundContinuation{});
    }
    // chain_gen|tip_digest|parent_entry_id|reader_token
    const auto first = token->find('|');
    const auto second =
        first == std::string::npos ? std::string::npos : token->find('|', first + 1U);
    const auto third =
        second == std::string::npos ? std::string::npos : token->find('|', second + 1U);
    if (first == std::string::npos || second == std::string::npos || third == std::string::npos ||
        first == 0 || second <= first + 1U || third <= second + 1U || third + 1U >= token->size()) {
        return base::Result<BoundContinuation>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageTokenInvalid)});
    }
    BoundContinuation bound;
    bound.chain_generation = token->substr(0, first);
    bound.tip_index_generation = token->substr(first + 1U, second - first - 1U);
    bound.parent_entry_id = token->substr(second + 1U, third - second - 1U);
    bound.reader_token = token->substr(third + 1U);
    if (bound.parent_entry_id != expected_parent || bound.chain_generation.empty() ||
        bound.tip_index_generation.empty() || bound.reader_token.empty()) {
        return base::Result<BoundContinuation>::failure(
            {base::ErrorCode::kInvalidArgument, std::string(kMessageTokenInvalid)});
    }
    return base::Result<BoundContinuation>::success(std::move(bound));
}

[[nodiscard]] contracts::RecoveryPointEntrySummary
project_summary(const contracts::RecoveryPointEntrySummary& item,
                const contracts::FileEntryDesc* described) {
    contracts::RecoveryPointEntrySummary summary = item;
    if (described != nullptr) {
        summary.display_name = project_display_name(described->name);
        summary.entry_kind = described->kind;
        summary.logical_size_bytes = described->logical_size;
    }
    if (summary.display_name.empty()) {
        summary.display_name = ".";
    }
    return summary;
}

[[nodiscard]] base::Result<contracts::RecoveryPointEntryPage>
project_entry_page(adapters::personal_archive::PersonalFileArchiveChainReader& reader,
                   const contracts::ListRecoveryPointEntriesRequest& request,
                   const base::CancellationToken cancellation) {
    const auto tip_generation = reader.index_root_digest();
    const auto chain_generation = reader.chain_generation_digest();
    auto bound = decode_continuation(request.page.continuation_token, request.parent_entry_id);
    if (!bound) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(bound.error());
    }
    if (!bound.value().tip_index_generation.empty() &&
        (bound.value().tip_index_generation != tip_generation ||
         bound.value().chain_generation != chain_generation)) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(
            {base::ErrorCode::kConflict, std::string(kMessageTokenInvalid)});
    }
    auto parent_id = parse_u64_text(request.parent_entry_id);
    if (!parent_id) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(parent_id.error());
    }
    const auto maximum =
        request.page.maximum_results == 0
            ? contracts::kMaximumServicePageResults
            : (std::min)(request.page.maximum_results, contracts::kMaximumServicePageResults);
    std::optional<std::string> reader_token =
        bound.value().reader_token.empty() ? std::nullopt
                                           : std::optional(bound.value().reader_token);
    auto page = reader.list_children(parent_id.value(), maximum, reader_token, cancellation);
    if (!page) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(page.error());
    }
    contracts::RecoveryPointEntryPage response;
    response.repository_connection_id = request.repository_connection_id;
    response.recovery_point_id = request.recovery_point_id;
    response.parent_entry_id = request.parent_entry_id;
    response.index_generation = tip_generation;
    response.items.reserve(page.value().items.size());
    for (const auto& item : page.value().items) {
        auto entry_id = parse_u64_text(item.entry_id);
        if (!entry_id) {
            return base::Result<contracts::RecoveryPointEntryPage>::failure(entry_id.error());
        }
        auto described = reader.describe_entry(entry_id.value(), cancellation);
        if (!described) {
            return base::Result<contracts::RecoveryPointEntryPage>::failure(described.error());
        }
        response.items.push_back(project_summary(item, &described.value()));
    }
    auto continuation = encode_continuation(chain_generation, tip_generation,
                                             request.parent_entry_id, page.value().continuation_token);
    if (!continuation) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(continuation.error());
    }
    response.continuation_token = std::move(continuation).value();
    auto valid_page = contracts::validate_recovery_point_entry_page(response);
    if (!valid_page) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(valid_page.error());
    }
    return base::Result<contracts::RecoveryPointEntryPage>::success(std::move(response));
}

} // namespace

base::Result<contracts::RecoveryPointEntryPage>
list_recovery_point_entries(ports::IControlPlaneDatabase& control_plane,
                            ports::IRepositoryStorageFactory& storage_factory,
                            const contracts::ListRecoveryPointEntriesRequest& request,
                            const base::CancellationToken cancellation) {
    if (auto valid = contracts::validate_list_recovery_point_entries_request(request); !valid) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(valid.error());
    }
    if (!request.repository_connection_id || request.repository_connection_id->empty()) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(
            {base::ErrorCode::kInvalidArgument, "repository_connection_id is required"});
    }
    auto chain = open_file_recovery_chain(control_plane, storage_factory,
                                          *request.repository_connection_id,
                                          request.recovery_point_id, request.archive_secret_ref,
                                          cancellation);
    if (!chain) {
        return base::Result<contracts::RecoveryPointEntryPage>::failure(chain.error());
    }
    return project_entry_page(*chain.value().reader, request, cancellation);
}

} // namespace aegra::apps::service
