#pragma once

#include "aegra/base/cancellation.h"
#include "aegra/base/result.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace aegra::ports {

inline constexpr std::size_t kMaximumObjectKeyBytes = 1'024;
inline constexpr std::uint32_t kMaximumObjectListResults = 10'000;

namespace object_storage_detail {

[[nodiscard]] constexpr bool valid_segment(const std::string_view segment) noexcept {
    if (segment.empty() || segment == "." || segment == "..") {
        return false;
    }
    for (const char character : segment) {
        const auto byte = static_cast<unsigned char>(character);
        if (character == '\\' || character == ':' || byte < 0x20 || byte == 0x7F) {
            return false;
        }
    }
    return true;
}

} // namespace object_storage_detail

[[nodiscard]] constexpr bool is_valid_object_key(const std::string_view key) noexcept {
    if (key.empty() || key.size() > kMaximumObjectKeyBytes || key.front() == '/' ||
        key.back() == '/') {
        return false;
    }
    std::size_t begin = 0;
    while (begin < key.size()) {
        const auto end = key.find('/', begin);
        const auto segment =
            key.substr(begin, end == std::string_view::npos ? key.size() - begin : end - begin);
        if (!object_storage_detail::valid_segment(segment)) {
            return false;
        }
        if (end == std::string_view::npos) {
            break;
        }
        begin = end + 1;
    }
    return true;
}

[[nodiscard]] constexpr bool is_valid_object_prefix(const std::string_view prefix) noexcept {
    if (prefix.empty() || prefix.size() > kMaximumObjectKeyBytes || prefix.front() == '/') {
        return false;
    }
    return prefix.back() == '/' ? is_valid_object_key(prefix.substr(0, prefix.size() - 1))
                                : is_valid_object_key(prefix);
}

struct ObjectAttributes final {
    std::string key;
    std::uint64_t size_bytes{0};
    std::string generation;

    [[nodiscard]] bool operator==(const ObjectAttributes&) const = default;
};

struct ObjectStorageCapabilities final {
    bool atomic_rename_publish{false};
    bool conditional_create{false};
    bool strong_read_after_write{false};
    bool strong_list_consistency{false};

    [[nodiscard]] bool operator==(const ObjectStorageCapabilities&) const noexcept = default;
};

struct ObjectListRequest final {
    std::string prefix;
    std::optional<std::string> continuation_token;
    std::uint32_t maximum_results{1'000};
};

struct ObjectListPage final {
    std::vector<ObjectAttributes> objects;
    std::optional<std::string> continuation_token;
};

enum class PublishCondition : std::uint8_t {
    kCreateOnly = 1,
    kReplaceIfGenerationMatches = 2,
};

struct ObjectPublishRequest final {
    std::string staging_key;
    std::string destination_key;
    PublishCondition condition{PublishCondition::kCreateOnly};
    std::optional<std::string> expected_generation;
};

struct ObjectDeleteRequest final {
    std::string key;
    std::string operation_id;
    std::optional<std::string> expected_generation;
};

class IObjectReader {
  public:
    IObjectReader() = default;
    virtual ~IObjectReader() = default;
    IObjectReader(const IObjectReader&) = delete;
    IObjectReader& operator=(const IObjectReader&) = delete;
    IObjectReader(IObjectReader&&) = delete;
    IObjectReader& operator=(IObjectReader&&) = delete;

    // Returned attributes are a point-in-time snapshot. Implementations may short-read ranges.
    [[nodiscard]] virtual base::Result<ObjectAttributes>
    get_attributes(std::string_view key, base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<std::size_t>
    read_range(std::string_view key, std::uint64_t offset, std::span<std::byte> destination,
               base::CancellationToken cancellation) = 0;
};

class IStagedObjectWriteSession {
  public:
    IStagedObjectWriteSession() = default;
    virtual ~IStagedObjectWriteSession() = default;
    IStagedObjectWriteSession(const IStagedObjectWriteSession&) = delete;
    IStagedObjectWriteSession& operator=(const IStagedObjectWriteSession&) = delete;
    IStagedObjectWriteSession(IStagedObjectWriteSession&&) = delete;
    IStagedObjectWriteSession& operator=(IStagedObjectWriteSession&&) = delete;

    // Source is consumed or copied before write returns. A non-completed session must abort.
    [[nodiscard]] virtual base::Result<void> write(std::span<const std::byte> source,
                                                   base::CancellationToken cancellation) = 0;
    [[nodiscard]] virtual base::Result<void> complete(base::CancellationToken cancellation) = 0;
    virtual void abort() noexcept = 0;
};

class IStagedObjectWriter {
  public:
    IStagedObjectWriter() = default;
    virtual ~IStagedObjectWriter() = default;
    IStagedObjectWriter(const IStagedObjectWriter&) = delete;
    IStagedObjectWriter& operator=(const IStagedObjectWriter&) = delete;
    IStagedObjectWriter(IStagedObjectWriter&&) = delete;
    IStagedObjectWriter& operator=(IStagedObjectWriter&&) = delete;

    [[nodiscard]] virtual base::Result<std::unique_ptr<IStagedObjectWriteSession>>
    begin_staged_write(std::string_view staging_key, base::CancellationToken cancellation) = 0;
};

class IPrefixEnumerator {
  public:
    IPrefixEnumerator() = default;
    virtual ~IPrefixEnumerator() = default;
    IPrefixEnumerator(const IPrefixEnumerator&) = delete;
    IPrefixEnumerator& operator=(const IPrefixEnumerator&) = delete;
    IPrefixEnumerator(IPrefixEnumerator&&) = delete;
    IPrefixEnumerator& operator=(IPrefixEnumerator&&) = delete;

    // Continuation tokens are opaque and only valid with the originating prefix.
    [[nodiscard]] virtual base::Result<ObjectListPage>
    enumerate(const ObjectListRequest& request, base::CancellationToken cancellation) = 0;
};

class IObjectPublisher {
  public:
    IObjectPublisher() = default;
    virtual ~IObjectPublisher() = default;
    IObjectPublisher(const IObjectPublisher&) = delete;
    IObjectPublisher& operator=(const IObjectPublisher&) = delete;
    IObjectPublisher(IObjectPublisher&&) = delete;
    IObjectPublisher& operator=(IObjectPublisher&&) = delete;

    // kOutcomeUnknown requires reconciliation through IObjectReader before retrying.
    [[nodiscard]] virtual base::Result<ObjectAttributes>
    publish(const ObjectPublishRequest& request, base::CancellationToken cancellation) = 0;
};

class IObjectDeleter {
  public:
    IObjectDeleter() = default;
    virtual ~IObjectDeleter() = default;
    IObjectDeleter(const IObjectDeleter&) = delete;
    IObjectDeleter& operator=(const IObjectDeleter&) = delete;
    IObjectDeleter(IObjectDeleter&&) = delete;
    IObjectDeleter& operator=(IObjectDeleter&&) = delete;

    // Missing objects are successful. An operation ID is immutable across retries.
    [[nodiscard]] virtual base::Result<void>
    delete_object(const ObjectDeleteRequest& request, base::CancellationToken cancellation) = 0;
};

class IObjectStorageCapabilities {
  public:
    IObjectStorageCapabilities() = default;
    virtual ~IObjectStorageCapabilities() = default;
    IObjectStorageCapabilities(const IObjectStorageCapabilities&) = delete;
    IObjectStorageCapabilities& operator=(const IObjectStorageCapabilities&) = delete;
    IObjectStorageCapabilities(IObjectStorageCapabilities&&) = delete;
    IObjectStorageCapabilities& operator=(IObjectStorageCapabilities&&) = delete;

    [[nodiscard]] virtual ObjectStorageCapabilities capabilities() const noexcept = 0;
};

} // namespace aegra::ports
