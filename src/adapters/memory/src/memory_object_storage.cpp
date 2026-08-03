#include "aegra/adapters/memory/memory_object_storage.h"

#include <algorithm>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace aegra::adapters::memory {
namespace detail {

enum class StagedStatus : std::uint8_t {
    kWriting = 1,
    kComplete = 2,
};

struct StagedObject final {
    std::vector<std::byte> data;
    StagedStatus status{StagedStatus::kWriting};
};

struct StoredObject final {
    std::vector<std::byte> data;
    std::string generation;
};

struct DeleteOperation final {
    std::optional<std::string> expected_generation;
};

struct MemoryObjectStorageState final {
    mutable std::mutex mutex;
    MemoryObjectStorageOptions options;
    std::map<std::string, StagedObject, std::less<>> staged_objects;
    std::map<std::string, StoredObject, std::less<>> objects;
    std::map<std::pair<std::string, std::string>, DeleteOperation> delete_operations;
    std::uint64_t next_generation{1};
};

} // namespace detail
namespace {

constexpr std::size_t kMaximumObjectKeyBytes = 1'024;

[[nodiscard]] base::Error error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] bool valid_segment(const std::string_view segment) noexcept {
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }
    return std::ranges::none_of(segment, [](const char character) {
        const auto byte = static_cast<unsigned char>(character);
        return character == '\\' || character == ':' || byte < 0x20 || byte == 0x7F;
    });
}

[[nodiscard]] bool valid_key(const std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaximumObjectKeyBytes || key.front() == '/' ||
        key.back() == '/') {
        return false;
    }
    std::size_t begin = 0;
    while (begin < key.size()) {
        const auto end = key.find('/', begin);
        const auto segment =
            key.substr(begin, end == std::string_view::npos ? key.size() - begin : end - begin);
        if (!valid_segment(segment)) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

[[nodiscard]] bool valid_prefix(const std::string_view prefix) noexcept {
    if (prefix.empty() || prefix.size() > kMaximumObjectKeyBytes || prefix.front() == '/') {
        return false;
    }
    return prefix.back() == '/' ? valid_key(prefix.substr(0, prefix.size() - 1))
                                : valid_key(prefix);
}

[[nodiscard]] base::Result<void> check_cancelled(const base::CancellationToken cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kCancelled, "object storage operation cancelled"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] ports::ObjectAttributes attributes(const std::string& key,
                                                 const detail::StoredObject& object) {
    return {key, static_cast<std::uint64_t>(object.data.size()), object.generation};
}

class MemoryStagedObjectWriteSession final : public ports::IStagedObjectWriteSession {
  public:
    MemoryStagedObjectWriteSession(std::shared_ptr<detail::MemoryObjectStorageState> state,
                                   std::string key)
        : state_(std::move(state)), key_(std::move(key)) {}

    ~MemoryStagedObjectWriteSession() override { abort(); }

    [[nodiscard]] base::Result<void> write(std::span<const std::byte> source,
                                           base::CancellationToken cancellation) override;
    [[nodiscard]] base::Result<void> complete(base::CancellationToken cancellation) override;
    void abort() noexcept override;

  private:
    std::shared_ptr<detail::MemoryObjectStorageState> state_;
    std::string key_;
    bool finished_{false};
};

base::Result<void>
MemoryStagedObjectWriteSession::write(const std::span<const std::byte> source,
                                      const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return active;
    }
    const std::scoped_lock lock(state_->mutex);
    const auto found = state_->staged_objects.find(key_);
    if (finished_ || found == state_->staged_objects.end() ||
        found->second.status != detail::StagedStatus::kWriting) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "staged object is not writable"));
    }
    auto& data = found->second.data;
    const auto failure_offset = state_->options.fail_write_after_bytes;
    if (failure_offset &&
        (data.size() >= *failure_offset || source.size() > *failure_offset - data.size())) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "injected staged object write failure"));
    }
    data.insert(data.end(), source.begin(), source.end());
    return base::Result<void>::success();
}

base::Result<void>
MemoryStagedObjectWriteSession::complete(const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return active;
    }
    const std::scoped_lock lock(state_->mutex);
    const auto found = state_->staged_objects.find(key_);
    if (finished_ || found == state_->staged_objects.end() ||
        found->second.status != detail::StagedStatus::kWriting) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "staged object cannot be completed"));
    }
    if (state_->options.fail_complete) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "injected staged object completion failure"));
    }
    found->second.status = detail::StagedStatus::kComplete;
    finished_ = true;
    return base::Result<void>::success();
}

void MemoryStagedObjectWriteSession::abort() noexcept {
    if (finished_) {
        return;
    }
    const std::scoped_lock lock(state_->mutex);
    state_->staged_objects.erase(key_);
    finished_ = true;
}

[[nodiscard]] bool valid_publish_request(const ports::ObjectPublishRequest& request) noexcept {
    if (!valid_key(request.staging_key) || !valid_key(request.destination_key) ||
        request.staging_key == request.destination_key) {
        return false;
    }
    if (request.condition == ports::PublishCondition::kCreateOnly) {
        return !request.expected_generation.has_value();
    }
    return request.condition == ports::PublishCondition::kReplaceIfGenerationMatches &&
           request.expected_generation.has_value() && !request.expected_generation->empty();
}

[[nodiscard]] bool same_delete_operation(const detail::DeleteOperation& existing,
                                         const ports::ObjectDeleteRequest& request) {
    return existing.expected_generation == request.expected_generation;
}

} // namespace

MemoryObjectStorage::MemoryObjectStorage(MemoryObjectStorageOptions options)
    : state_(std::make_shared<detail::MemoryObjectStorageState>()) {
    if (options.maximum_read_size == 0) {
        options.maximum_read_size = 1;
    }
    state_->options = std::move(options);
}

MemoryObjectStorage::~MemoryObjectStorage() = default;

base::Result<ports::ObjectAttributes>
MemoryObjectStorage::get_attributes(const std::string_view key,
                                    const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<ports::ObjectAttributes>::failure(active.error());
    }
    if (!valid_key(key)) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kInvalidArgument, "object key is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    const auto found = state_->objects.find(key);
    if (found == state_->objects.end()) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kNotFound, "object was not found"));
    }
    return base::Result<ports::ObjectAttributes>::success(attributes(found->first, found->second));
}

base::Result<std::size_t>
MemoryObjectStorage::read_range(const std::string_view key, const std::uint64_t offset,
                                const std::span<std::byte> destination,
                                const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<std::size_t>::failure(active.error());
    }
    if (!valid_key(key)) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "object key is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    const auto found = state_->objects.find(key);
    if (found == state_->objects.end()) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kNotFound, "object was not found"));
    }
    const auto& data = found->second.data;
    if (offset > data.size()) {
        return base::Result<std::size_t>::failure(
            error(base::ErrorCode::kInvalidArgument, "object read offset is out of range"));
    }
    const auto available = data.size() - static_cast<std::size_t>(offset);
    const auto count =
        (std::min)({destination.size(), available, state_->options.maximum_read_size});
    const auto source =
        std::span<const std::byte>(data).subspan(static_cast<std::size_t>(offset), count);
    std::ranges::copy(source, destination.begin());
    return base::Result<std::size_t>::success(count);
}

base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>
MemoryObjectStorage::begin_staged_write(const std::string_view staging_key,
                                        const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            active.error());
    }
    if (!valid_key(staging_key)) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            error(base::ErrorCode::kInvalidArgument, "staging key is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    if (state_->objects.contains(staging_key) || state_->staged_objects.contains(staging_key)) {
        return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::failure(
            error(base::ErrorCode::kConflict, "staging key already exists"));
    }
    state_->staged_objects.emplace(std::string(staging_key), detail::StagedObject{});
    return base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>::success(
        std::make_unique<MemoryStagedObjectWriteSession>(state_, std::string(staging_key)));
}

base::Result<ports::ObjectListPage>
MemoryObjectStorage::enumerate(const ports::ObjectListRequest& request,
                               const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<ports::ObjectListPage>::failure(active.error());
    }
    if (!valid_prefix(request.prefix) || request.maximum_results == 0 ||
        (request.continuation_token &&
         (!valid_key(*request.continuation_token) ||
          !request.continuation_token->starts_with(request.prefix)))) {
        return base::Result<ports::ObjectListPage>::failure(
            error(base::ErrorCode::kInvalidArgument, "object list request is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    auto iterator = request.continuation_token
                        ? state_->objects.upper_bound(*request.continuation_token)
                        : state_->objects.lower_bound(request.prefix);
    ports::ObjectListPage page;
    while (iterator != state_->objects.end() && iterator->first.starts_with(request.prefix) &&
           page.objects.size() < request.maximum_results) {
        page.objects.push_back(attributes(iterator->first, iterator->second));
        ++iterator;
    }
    if (!page.objects.empty() && iterator != state_->objects.end() &&
        iterator->first.starts_with(request.prefix)) {
        page.continuation_token = page.objects.back().key;
    }
    return base::Result<ports::ObjectListPage>::success(std::move(page));
}

base::Result<ports::ObjectAttributes>
MemoryObjectStorage::publish(const ports::ObjectPublishRequest& request,
                             const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return base::Result<ports::ObjectAttributes>::failure(active.error());
    }
    if (!valid_publish_request(request)) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kInvalidArgument, "object publish request is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    const auto staged = state_->staged_objects.find(request.staging_key);
    if (staged == state_->staged_objects.end() ||
        staged->second.status != detail::StagedStatus::kComplete) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kConflict, "staged object is not complete"));
    }
    if (state_->options.fail_publish_destination_key == request.destination_key) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kIoFailure, "injected object publish failure"));
    }
    const auto existing = state_->objects.find(request.destination_key);
    if (request.condition == ports::PublishCondition::kCreateOnly &&
        existing != state_->objects.end()) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kConflict, "published object already exists"));
    }
    if (request.condition == ports::PublishCondition::kReplaceIfGenerationMatches &&
        (existing == state_->objects.end() ||
         existing->second.generation != *request.expected_generation)) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kConflict, "published object generation changed"));
    }
    const auto generation = std::to_string(state_->next_generation++);
    auto data = std::move(staged->second.data);
    state_->staged_objects.erase(staged);
    auto inserted = state_->objects.insert_or_assign(
        request.destination_key, detail::StoredObject{std::move(data), generation});
    auto result = attributes(inserted.first->first, inserted.first->second);
    if (state_->options.unknown_publish_destination_key == request.destination_key) {
        return base::Result<ports::ObjectAttributes>::failure(
            error(base::ErrorCode::kOutcomeUnknown, "object publish outcome is unknown"));
    }
    return base::Result<ports::ObjectAttributes>::success(std::move(result));
}

base::Result<void> MemoryObjectStorage::delete_object(const ports::ObjectDeleteRequest& request,
                                                      const base::CancellationToken cancellation) {
    auto active = check_cancelled(cancellation);
    if (!active) {
        return active;
    }
    if (!valid_key(request.key) || request.operation_id.empty()) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kInvalidArgument, "object delete request is invalid"));
    }
    const std::scoped_lock lock(state_->mutex);
    const auto operation_key = std::pair(request.operation_id, request.key);
    const auto prior = state_->delete_operations.find(operation_key);
    if (prior != state_->delete_operations.end()) {
        return same_delete_operation(prior->second, request)
                   ? base::Result<void>::success()
                   : base::Result<void>::failure(
                         error(base::ErrorCode::kConflict, "delete operation changed on retry"));
    }
    if (state_->options.fail_delete_key == request.key) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kIoFailure, "injected object delete failure"));
    }
    const auto existing = state_->objects.find(request.key);
    if (existing != state_->objects.end() && request.expected_generation &&
        existing->second.generation != *request.expected_generation) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kConflict, "deleted object generation changed"));
    }
    if (existing != state_->objects.end()) {
        state_->objects.erase(existing);
    }
    state_->delete_operations.emplace(std::move(operation_key),
                                      detail::DeleteOperation{request.expected_generation});
    if (state_->options.unknown_delete_key == request.key) {
        return base::Result<void>::failure(
            error(base::ErrorCode::kOutcomeUnknown, "object delete outcome is unknown"));
    }
    return base::Result<void>::success();
}

ports::ObjectStorageCapabilities MemoryObjectStorage::capabilities() const noexcept {
    return {
        .atomic_rename_publish = true,
        .conditional_create = true,
        .strong_read_after_write = true,
        .strong_list_consistency = true,
    };
}

} // namespace aegra::adapters::memory
