#include "aegra/adapters/storage_local/local_object_storage.h"

#include "local_storage_internal.h"

#include <algorithm>
#include <filesystem>
#include <limits>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <string>
#include <utility>

namespace aegra::adapters::storage_local {
namespace {

[[nodiscard]] base::Result<void> validate_list_request(const ports::ObjectListRequest& request) {
    if (!detail::is_local_object_prefix(request.prefix) || request.maximum_results == 0 ||
        request.maximum_results > ports::kMaximumObjectListResults ||
        (request.continuation_token &&
         (!detail::is_local_object_key(*request.continuation_token) ||
          !request.continuation_token->starts_with(request.prefix)))) {
        return base::Result<void>::failure(detail::local_error(
            base::ErrorCode::kInvalidArgument, "local object list request is invalid"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool is_internal_path(const detail::LocalObjectStorageState& state,
                                    const std::filesystem::path& path) {
    const auto relative = path.lexically_relative(state.root);
    return !relative.empty() && detail::is_internal_component(relative.begin()->native());
}

[[nodiscard]] base::Result<std::set<std::string, std::less<>>>
collect_page_keys(const detail::LocalObjectStorageState& state,
                  const ports::ObjectListRequest& request,
                  const base::CancellationToken cancellation) {
    std::set<std::string, std::less<>> keys;
    std::error_code filesystem_error;
    std::filesystem::recursive_directory_iterator iterator(state.root, filesystem_error);
    const std::filesystem::recursive_directory_iterator end;
    while (!filesystem_error && iterator != end) {
        auto active = detail::check_cancelled(cancellation);
        if (!active) {
            return base::Result<std::set<std::string, std::less<>>>::failure(active.error());
        }
        const auto path = iterator->path();
        const auto attributes = GetFileAttributesW(path.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES) {
            return base::Result<std::set<std::string, std::less<>>>::failure(
                detail::win32_error(GetLastError(), "enumerate local object"));
        }
        const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (directory &&
            (is_internal_path(state, path) || (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0)) {
            iterator.disable_recursion_pending();
        }
        if (!is_internal_path(state, path) && (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0) {
            return base::Result<std::set<std::string, std::less<>>>::failure(detail::local_error(
                base::ErrorCode::kConflict, "local object tree contains a reparse point"));
        }
        if (!directory && !is_internal_path(state, path)) {
            auto key = detail::object_key(state, path);
            if (!key) {
                return base::Result<std::set<std::string, std::less<>>>::failure(key.error());
            }
            const bool after_token =
                !request.continuation_token || key.value() > *request.continuation_token;
            if (after_token && key.value().starts_with(request.prefix)) {
                keys.insert(std::move(key).value());
                if (keys.size() > static_cast<std::size_t>(request.maximum_results) + 1U) {
                    keys.erase(std::prev(keys.end()));
                }
            }
        }
        iterator.increment(filesystem_error);
    }
    if (filesystem_error) {
        return base::Result<std::set<std::string, std::less<>>>::failure(
            detail::local_error(base::ErrorCode::kIoFailure, "enumerate local object tree failed"));
    }
    return base::Result<std::set<std::string, std::less<>>>::success(std::move(keys));
}

[[nodiscard]] base::Result<ports::ObjectListPage>
make_page(const detail::LocalObjectStorageState& state, const ports::ObjectListRequest& request,
          const std::set<std::string, std::less<>>& keys) {
    ports::ObjectListPage page;
    page.objects.reserve(
        (std::min)(keys.size(), static_cast<std::size_t>(request.maximum_results)));
    for (const auto& key : keys) {
        if (page.objects.size() == request.maximum_results) {
            break;
        }
        auto path = detail::object_path(state, key);
        auto attributes = path ? detail::attributes_for_path(state, key, path.value())
                               : base::Result<ports::ObjectAttributes>::failure(path.error());
        if (!attributes) {
            return base::Result<ports::ObjectListPage>::failure(attributes.error());
        }
        page.objects.push_back(std::move(attributes).value());
    }
    if (keys.size() > request.maximum_results && !page.objects.empty()) {
        page.continuation_token = page.objects.back().key;
    }
    return base::Result<ports::ObjectListPage>::success(std::move(page));
}

[[nodiscard]] bool valid_publish_request(const ports::ObjectPublishRequest& request) {
    if (!detail::is_local_object_key(request.staging_key) ||
        !detail::is_local_object_key(request.destination_key) ||
        request.staging_key == request.destination_key) {
        return false;
    }
    if (request.condition == ports::PublishCondition::kCreateOnly) {
        return !request.expected_generation.has_value();
    }
    return request.condition == ports::PublishCondition::kReplaceIfGenerationMatches &&
           request.expected_generation && !request.expected_generation->empty();
}

[[nodiscard]] base::Result<void> publish_file(const detail::LocalObjectStorageState& state,
                                              const ports::ObjectPublishRequest& request,
                                              const std::filesystem::path& staging,
                                              const std::filesystem::path& destination) {
    if (request.condition == ports::PublishCondition::kCreateOnly) {
        if (!MoveFileExW(staging.c_str(), destination.c_str(), MOVEFILE_WRITE_THROUGH)) {
            return base::Result<void>::failure(
                detail::win32_error(GetLastError(), "publish local object"));
        }
        return base::Result<void>::success();
    }
    auto current = detail::attributes_for_path(state, request.destination_key, destination);
    if (!current || current.value().generation != *request.expected_generation) {
        return base::Result<void>::failure(
            detail::local_error(base::ErrorCode::kConflict, "local object generation changed"));
    }
    if (!ReplaceFileW(destination.c_str(), staging.c_str(), nullptr, 0, nullptr, nullptr)) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "replace local object"));
    }
    return base::Result<void>::success();
}

[[nodiscard]] bool same_delete_operation(const detail::DeleteOperation& existing,
                                         const ports::ObjectDeleteRequest& request) {
    return existing.expected_generation == request.expected_generation;
}

} // namespace

LocalObjectStorage::LocalObjectStorage(
    std::shared_ptr<detail::LocalObjectStorageState> state) noexcept
    : state_(std::move(state)) {}

LocalObjectStorage::~LocalObjectStorage() = default;

base::Result<std::unique_ptr<LocalObjectStorage>>
LocalObjectStorage::open(const LocalObjectStorageOpenRequest& request) {
    auto state = detail::open_local_state(request);
    if (!state) {
        return base::Result<std::unique_ptr<LocalObjectStorage>>::failure(state.error());
    }
    return base::Result<std::unique_ptr<LocalObjectStorage>>::success(
        std::unique_ptr<LocalObjectStorage>(new LocalObjectStorage(std::move(state).value())));
}

base::Result<ports::ObjectAttributes>
LocalObjectStorage::get_attributes(const std::string_view key,
                                   const base::CancellationToken cancellation) {
    auto active = detail::check_cancelled(cancellation);
    auto path = active ? detail::object_path(*state_, key)
                       : base::Result<std::filesystem::path>::failure(active.error());
    if (!path) {
        return base::Result<ports::ObjectAttributes>::failure(path.error());
    }
    const std::shared_lock lock(state_->mutex);
    return detail::attributes_for_path(*state_, key, path.value());
}

base::Result<std::size_t>
LocalObjectStorage::read_range(const std::string_view key, const std::uint64_t offset,
                               const std::span<std::byte> destination,
                               const base::CancellationToken cancellation) {
    auto active = detail::check_cancelled(cancellation);
    auto path = active ? detail::object_path(*state_, key)
                       : base::Result<std::filesystem::path>::failure(active.error());
    if (!path) {
        return base::Result<std::size_t>::failure(path.error());
    }
    const std::shared_lock lock(state_->mutex);
    auto handle = detail::open_regular_file(*state_, path.value(), GENERIC_READ);
    auto attributes = handle ? detail::attributes_from_handle(key, handle.value().get())
                             : base::Result<ports::ObjectAttributes>::failure(handle.error());
    if (!attributes) {
        return base::Result<std::size_t>::failure(attributes.error());
    }
    if (offset > attributes.value().size_bytes ||
        offset > static_cast<std::uint64_t>((std::numeric_limits<LONGLONG>::max)())) {
        return base::Result<std::size_t>::failure(detail::local_error(
            base::ErrorCode::kInvalidArgument, "local object read offset is invalid"));
    }
    const auto available = attributes.value().size_bytes - offset;
    const auto count64 =
        (std::min)({available, static_cast<std::uint64_t>(destination.size()),
                    static_cast<std::uint64_t>(state_->maximum_read_size),
                    static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)())});
    if (count64 == 0) {
        return base::Result<std::size_t>::success(0);
    }
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(handle.value().get(), position, nullptr, FILE_BEGIN)) {
        return base::Result<std::size_t>::failure(
            detail::win32_error(GetLastError(), "seek local object"));
    }
    DWORD actual = 0;
    if (!ReadFile(handle.value().get(), destination.data(), static_cast<DWORD>(count64), &actual,
                  nullptr)) {
        return base::Result<std::size_t>::failure(
            detail::win32_error(GetLastError(), "read local object"));
    }
    return base::Result<std::size_t>::success(actual);
}

base::Result<std::unique_ptr<ports::IStagedObjectWriteSession>>
LocalObjectStorage::begin_staged_write(const std::string_view staging_key,
                                       const base::CancellationToken cancellation) {
    return detail::create_staged_write_session(state_, staging_key, cancellation);
}

base::Result<ports::ObjectListPage>
LocalObjectStorage::enumerate(const ports::ObjectListRequest& request,
                              const base::CancellationToken cancellation) {
    auto valid = validate_list_request(request);
    auto active = valid ? detail::check_cancelled(cancellation) : valid;
    if (!active) {
        return base::Result<ports::ObjectListPage>::failure(active.error());
    }
    const std::shared_lock lock(state_->mutex);
    auto keys = collect_page_keys(*state_, request, cancellation);
    return keys ? make_page(*state_, request, keys.value())
                : base::Result<ports::ObjectListPage>::failure(keys.error());
}

base::Result<ports::ObjectAttributes>
LocalObjectStorage::publish(const ports::ObjectPublishRequest& request,
                            const base::CancellationToken cancellation) {
    auto active = detail::check_cancelled(cancellation);
    if (!active || !valid_publish_request(request)) {
        return base::Result<ports::ObjectAttributes>::failure(
            !active ? active.error()
                    : detail::local_error(base::ErrorCode::kInvalidArgument,
                                          "local object publish request is invalid"));
    }
    auto staging = detail::object_path(*state_, request.staging_key);
    auto destination = detail::object_path(*state_, request.destination_key);
    if (!staging || !destination) {
        return base::Result<ports::ObjectAttributes>::failure(!staging ? staging.error()
                                                                       : destination.error());
    }
    const std::unique_lock lock(state_->mutex);
    auto staged = detail::attributes_for_path(*state_, request.staging_key, staging.value());
    auto parents = detail::ensure_safe_parent_directories(*state_, destination.value());
    if (!staged || !parents) {
        return base::Result<ports::ObjectAttributes>::failure(!staged ? staged.error()
                                                                      : parents.error());
    }
    auto published = publish_file(*state_, request, staging.value(), destination.value());
    if (!published) {
        return base::Result<ports::ObjectAttributes>::failure(published.error());
    }
    auto attributes =
        detail::attributes_for_path(*state_, request.destination_key, destination.value());
    return attributes ? attributes
                      : base::Result<ports::ObjectAttributes>::failure(detail::local_error(
                            base::ErrorCode::kOutcomeUnknown,
                            "local object publish outcome requires reconciliation"));
}

base::Result<void> LocalObjectStorage::delete_object(const ports::ObjectDeleteRequest& request,
                                                     const base::CancellationToken cancellation) {
    auto active = detail::check_cancelled(cancellation);
    if (!active || !detail::is_local_object_key(request.key) || request.operation_id.empty()) {
        return !active ? active
                       : base::Result<void>::failure(
                             detail::local_error(base::ErrorCode::kInvalidArgument,
                                                 "local object delete request is invalid"));
    }
    auto path = detail::object_path(*state_, request.key);
    if (!path) {
        return base::Result<void>::failure(path.error());
    }
    const std::unique_lock lock(state_->mutex);
    const auto operation_key = std::pair(request.operation_id, request.key);
    const auto prior = state_->delete_operations.find(operation_key);
    if (prior != state_->delete_operations.end()) {
        return same_delete_operation(prior->second, request)
                   ? base::Result<void>::success()
                   : base::Result<void>::failure(detail::local_error(
                         base::ErrorCode::kConflict, "local delete operation changed on retry"));
    }
    auto handle = detail::open_regular_file(*state_, path.value(), DELETE);
    if (!handle && handle.error().code == base::ErrorCode::kNotFound) {
        state_->delete_operations.emplace(operation_key,
                                          detail::DeleteOperation{request.expected_generation});
        return base::Result<void>::success();
    }
    if (!handle) {
        return base::Result<void>::failure(handle.error());
    }
    auto attributes = detail::attributes_from_handle(request.key, handle.value().get());
    if (!attributes) {
        return base::Result<void>::failure(attributes.error());
    }
    if (request.expected_generation &&
        attributes.value().generation != *request.expected_generation) {
        return base::Result<void>::failure(
            detail::local_error(base::ErrorCode::kConflict, "local object generation changed"));
    }
    FILE_DISPOSITION_INFO disposition{TRUE};
    if (!SetFileInformationByHandle(handle.value().get(), FileDispositionInfo, &disposition,
                                    sizeof(disposition))) {
        return base::Result<void>::failure(
            detail::win32_error(GetLastError(), "delete local object"));
    }
    handle.value().reset();
    state_->delete_operations.emplace(operation_key,
                                      detail::DeleteOperation{request.expected_generation});
    return base::Result<void>::success();
}

ports::ObjectStorageCapabilities LocalObjectStorage::capabilities() const noexcept {
    return {
        .atomic_rename_publish = true,
        .conditional_create = true,
        .strong_read_after_write = true,
        .strong_list_consistency = true,
    };
}

} // namespace aegra::adapters::storage_local
