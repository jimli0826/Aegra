#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>
#include <wincrypt.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_system {
namespace {

constexpr std::string_view kDpapiLocalMachinePrefix = "dpapi-lm:";
constexpr std::size_t kMaximumSecretBytes = 32;
constexpr std::size_t kMaximumSecretBlobBytes = 1'024;
constexpr std::size_t kMaximumEntropyIdBytes = 128;
// DPAPI envelope + base64 expansion for at most 1024 material bytes stays well under this.
constexpr std::size_t kMaximumProtectedBase64Bytes = 8'192;
constexpr DWORD kDpapiFlags = CRYPTPROTECT_LOCAL_MACHINE | CRYPTPROTECT_UI_FORBIDDEN;

struct ParsedDpapiSecretRef final {
    std::string_view entropy_id;
    std::string_view encoded_ciphertext;
};

[[nodiscard]] bool valid_entropy_id(const std::string_view entropy_id) noexcept {
    if (entropy_id.empty() || entropy_id.size() > kMaximumEntropyIdBytes) {
        return false;
    }
    // Must not contain ':' so SecretRef dpapi-lm:<id>:<base64> parses unambiguously.
    for (const unsigned char character : entropy_id) {
        const bool ok = (character >= 'a' && character <= 'z') ||
                        (character >= '0' && character <= '9') || character == '.' ||
                        character == '_' || character == '-';
        if (!ok) {
            return false;
        }
    }
    return true;
}

[[nodiscard]] base::Result<ParsedDpapiSecretRef>
parse_dpapi_secret_ref(const std::string_view value) {
    if (!value.starts_with(kDpapiLocalMachinePrefix)) {
        return base::Result<ParsedDpapiSecretRef>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential reference is unsupported"});
    }
    const auto rest = value.substr(kDpapiLocalMachinePrefix.size());
    const auto separator = rest.find(':');
    if (separator == std::string_view::npos || separator == 0 ||
        separator + 1U >= rest.size()) {
        return base::Result<ParsedDpapiSecretRef>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential reference is unsupported"});
    }
    ParsedDpapiSecretRef parsed;
    parsed.entropy_id = rest.substr(0, separator);
    parsed.encoded_ciphertext = rest.substr(separator + 1U);
    if (!valid_entropy_id(parsed.entropy_id)) {
        return base::Result<ParsedDpapiSecretRef>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential reference is unsupported"});
    }
    return base::Result<ParsedDpapiSecretRef>::success(parsed);
}

class LockedAllocation final {
  public:
    explicit LockedAllocation(const std::size_t size) noexcept
        : data_(VirtualAlloc(nullptr, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE)),
          size_(size) {
        locked_ = data_ != nullptr && VirtualLock(data_, size_) != FALSE;
    }

    ~LockedAllocation() {
        if (data_ == nullptr) {
            return;
        }
        SecureZeroMemory(data_, size_);
        if (locked_) {
            VirtualUnlock(data_, size_);
        }
        VirtualFree(data_, 0, MEM_RELEASE);
    }

    LockedAllocation(const LockedAllocation&) = delete;
    LockedAllocation& operator=(const LockedAllocation&) = delete;
    LockedAllocation(LockedAllocation&&) = delete;
    LockedAllocation& operator=(LockedAllocation&&) = delete;

    [[nodiscard]] bool valid() const noexcept { return data_ != nullptr && locked_; }
    [[nodiscard]] void* get() const noexcept { return data_; }
    void dismiss() noexcept { data_ = nullptr; }

  private:
    void* data_{nullptr};
    std::size_t size_{0};
    bool locked_{false};
};

class LockedSecret final : public ports::IResolvedSecret {
  public:
    LockedSecret(char* data, const std::size_t size) noexcept : data_(data), size_(size) {}

    ~LockedSecret() override {
        if (data_ == nullptr) {
            return;
        }
        SecureZeroMemory(data_, size_);
        VirtualUnlock(data_, size_);
        VirtualFree(data_, 0, MEM_RELEASE);
    }

    LockedSecret(const LockedSecret&) = delete;
    LockedSecret& operator=(const LockedSecret&) = delete;
    LockedSecret(LockedSecret&&) = delete;
    LockedSecret& operator=(LockedSecret&&) = delete;

    [[nodiscard]] static base::Result<std::unique_ptr<ports::IResolvedSecret>>
    create(const std::span<const std::byte> value) {
        LockedAllocation allocation(value.size());
        if (!allocation.valid()) {
            return failure("secure credential locking failed");
        }
        std::memcpy(allocation.get(), value.data(), value.size());
        std::unique_ptr<ports::IResolvedSecret> result =
            std::make_unique<LockedSecret>(static_cast<char*>(allocation.get()), value.size());
        allocation.dismiss();
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::success(std::move(result));
    }

    [[nodiscard]] std::string_view view() const noexcept override { return {data_, size_}; }

  private:
    [[nodiscard]] static base::Result<std::unique_ptr<ports::IResolvedSecret>>
    failure(const char* message) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kInternal, message});
    }

    char* data_{nullptr};
    std::size_t size_{0};
};

class UniqueDataBlob final {
  public:
    UniqueDataBlob() = default;
    ~UniqueDataBlob() { reset(); }

    UniqueDataBlob(const UniqueDataBlob&) = delete;
    UniqueDataBlob& operator=(const UniqueDataBlob&) = delete;
    UniqueDataBlob(UniqueDataBlob&&) = delete;
    UniqueDataBlob& operator=(UniqueDataBlob&&) = delete;

    [[nodiscard]] DATA_BLOB* get() noexcept { return &blob_; }
    [[nodiscard]] std::span<const std::byte> bytes() const noexcept {
        if (blob_.pbData == nullptr || blob_.cbData == 0) {
            return {};
        }
        return {reinterpret_cast<const std::byte*>(blob_.pbData), blob_.cbData};
    }

    void reset() noexcept {
        if (blob_.pbData != nullptr) {
            SecureZeroMemory(blob_.pbData, blob_.cbData);
            LocalFree(blob_.pbData);
            blob_.pbData = nullptr;
            blob_.cbData = 0;
        }
    }

  private:
    DATA_BLOB blob_{};
};

[[nodiscard]] char base64_encode_digit(const unsigned value) noexcept {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    return kTable[value & 63U];
}

[[nodiscard]] int base64_decode_digit(const char value) noexcept {
    if (value >= 'A' && value <= 'Z') {
        return value - 'A';
    }
    if (value >= 'a' && value <= 'z') {
        return value - 'a' + 26;
    }
    if (value >= '0' && value <= '9') {
        return value - '0' + 52;
    }
    if (value == '+') {
        return 62;
    }
    if (value == '/') {
        return 63;
    }
    return -1;
}

[[nodiscard]] base::Result<std::string> base64_encode(const std::span<const std::byte> input) {
    if (input.empty()) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "protected secret is empty"});
    }
    std::string output;
    output.reserve(((input.size() + 2U) / 3U) * 4U);
    std::size_t index = 0;
    while (index + 2U < input.size()) {
        const auto b0 = static_cast<unsigned>(input[index]);
        const auto b1 = static_cast<unsigned>(input[index + 1U]);
        const auto b2 = static_cast<unsigned>(input[index + 2U]);
        output.push_back(base64_encode_digit(b0 >> 2U));
        output.push_back(base64_encode_digit(((b0 & 3U) << 4U) | (b1 >> 4U)));
        output.push_back(base64_encode_digit(((b1 & 15U) << 2U) | (b2 >> 6U)));
        output.push_back(base64_encode_digit(b2 & 63U));
        index += 3U;
    }
    const auto remaining = input.size() - index;
    if (remaining == 1U) {
        const auto b0 = static_cast<unsigned>(input[index]);
        output.push_back(base64_encode_digit(b0 >> 2U));
        output.push_back(base64_encode_digit((b0 & 3U) << 4U));
        output.push_back('=');
        output.push_back('=');
    } else if (remaining == 2U) {
        const auto b0 = static_cast<unsigned>(input[index]);
        const auto b1 = static_cast<unsigned>(input[index + 1U]);
        output.push_back(base64_encode_digit(b0 >> 2U));
        output.push_back(base64_encode_digit(((b0 & 3U) << 4U) | (b1 >> 4U)));
        output.push_back(base64_encode_digit((b1 & 15U) << 2U));
        output.push_back('=');
    }
    return base::Result<std::string>::success(std::move(output));
}

[[nodiscard]] base::Result<std::vector<std::byte>> base64_decode(const std::string_view input) {
    if (input.empty() || (input.size() % 4U) != 0U || input.size() > kMaximumProtectedBase64Bytes) {
        return base::Result<std::vector<std::byte>>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "protected secret encoding is invalid"});
    }
    std::size_t padding = 0;
    if (input.back() == '=') {
        padding = 1;
        if (input.size() >= 2U && input[input.size() - 2U] == '=') {
            padding = 2;
        }
    }
    std::vector<std::byte> output;
    output.reserve((input.size() / 4U) * 3U - padding);
    for (std::size_t index = 0; index < input.size(); index += 4U) {
        const auto c0 = base64_decode_digit(input[index]);
        const auto c1 = base64_decode_digit(input[index + 1U]);
        const auto c2 = input[index + 2U] == '=' ? 0 : base64_decode_digit(input[index + 2U]);
        const auto c3 = input[index + 3U] == '=' ? 0 : base64_decode_digit(input[index + 3U]);
        if (c0 < 0 || c1 < 0 || (input[index + 2U] != '=' && c2 < 0) ||
            (input[index + 3U] != '=' && c3 < 0)) {
            return base::Result<std::vector<std::byte>>::failure(base::Error{
                base::ErrorCode::kInvalidArgument, "protected secret encoding is invalid"});
        }
        output.push_back(static_cast<std::byte>((c0 << 2) | (c1 >> 4)));
        if (input[index + 2U] != '=') {
            output.push_back(static_cast<std::byte>(((c1 & 15) << 4) | (c2 >> 2)));
        }
        if (input[index + 3U] != '=') {
            output.push_back(static_cast<std::byte>(((c2 & 3) << 6) | c3));
        }
    }
    if (output.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "protected secret is empty"});
    }
    return base::Result<std::vector<std::byte>>::success(std::move(output));
}

[[nodiscard]] DATA_BLOB make_entropy_blob(const std::string_view entropy_id) noexcept {
    DATA_BLOB entropy{};
    // CryptProtectData requires non-const pbData; entropy bytes are not modified.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    entropy.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(entropy_id.data()));
    entropy.cbData = static_cast<DWORD>(entropy_id.size());
    return entropy;
}

[[nodiscard]] base::Result<std::string>
protect_bytes(const std::span<const std::byte> plaintext, const std::string_view entropy_id) {
    DATA_BLOB input{};
    // CryptProtectData requires non-const pbData; input is not modified.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    input.pbData = reinterpret_cast<BYTE*>(const_cast<std::byte*>(plaintext.data()));
    input.cbData = static_cast<DWORD>(plaintext.size());
    auto entropy = make_entropy_blob(entropy_id);
    UniqueDataBlob output;
    if (CryptProtectData(&input, L"aegra-archive", &entropy, nullptr, nullptr, kDpapiFlags,
                         output.get()) == FALSE) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kIoFailure, "DPAPI protect failed"});
    }
    return base64_encode(output.bytes());
}

[[nodiscard]] base::Result<std::vector<std::byte>>
unprotect_bytes(const std::span<const std::byte> ciphertext, const std::string_view entropy_id) {
    DATA_BLOB input{};
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast)
    input.pbData = reinterpret_cast<BYTE*>(const_cast<std::byte*>(ciphertext.data()));
    input.cbData = static_cast<DWORD>(ciphertext.size());
    auto entropy = make_entropy_blob(entropy_id);
    UniqueDataBlob output;
    if (CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr, kDpapiFlags,
                           output.get()) == FALSE) {
        return base::Result<std::vector<std::byte>>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "DPAPI unprotect failed"});
    }
    const auto plain = output.bytes();
    if (plain.empty()) {
        return base::Result<std::vector<std::byte>>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "DPAPI secret is empty"});
    }
    return base::Result<std::vector<std::byte>>::success(
        std::vector<std::byte>(plain.begin(), plain.end()));
}

} // namespace

base::Result<std::unique_ptr<ports::IResolvedSecret>>
WindowsCredentialResolver::resolve(const contracts::SecretRef& secret_ref,
                                   const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kCancelled, "credential resolution cancelled"});
    }
    auto parsed = parse_dpapi_secret_ref(secret_ref.value);
    if (!parsed) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(parsed.error());
    }
    auto ciphertext = base64_decode(parsed.value().encoded_ciphertext);
    if (!ciphertext) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(ciphertext.error());
    }
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kCancelled, "credential resolution cancelled"});
    }
    // Same UTF-8 entropy_id used at protect time (schedule_id for schedule passwords).
    auto plaintext = unprotect_bytes(ciphertext.value(), parsed.value().entropy_id);
    SecureZeroMemory(ciphertext.value().data(), ciphertext.value().size());
    if (!plaintext) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(plaintext.error());
    }
    auto locked = LockedSecret::create(plaintext.value());
    SecureZeroMemory(plaintext.value().data(), plaintext.value().size());
    return locked;
}

namespace {

[[nodiscard]] base::Result<contracts::SecretRef>
protect_secret_with_limit(const std::string_view secret_material, const std::string_view entropy_id,
                          const std::size_t maximum_bytes) {
    if (secret_material.empty() || secret_material.size() > maximum_bytes ||
        !valid_entropy_id(entropy_id)) {
        return base::Result<contracts::SecretRef>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "secret material is invalid"});
    }
    const auto bytes = std::as_bytes(std::span(secret_material.data(), secret_material.size()));
    auto encoded = protect_bytes(bytes, entropy_id);
    if (!encoded) {
        return base::Result<contracts::SecretRef>::failure(encoded.error());
    }
    if (encoded.value().size() > kMaximumProtectedBase64Bytes) {
        return base::Result<contracts::SecretRef>::failure(
            base::Error{base::ErrorCode::kInternal, "protected secret is too large"});
    }
    contracts::SecretRef reference;
    reference.value.reserve(kDpapiLocalMachinePrefix.size() + entropy_id.size() + 1U +
                            encoded.value().size());
    reference.value.append(kDpapiLocalMachinePrefix);
    reference.value.append(entropy_id);
    reference.value.push_back(':');
    reference.value.append(encoded.value());
    return base::Result<contracts::SecretRef>::success(std::move(reference));
}

} // namespace

base::Result<contracts::SecretRef>
protect_local_machine_secret(const std::string_view secret_material,
                             const std::string_view entropy_id) {
    return protect_secret_with_limit(secret_material, entropy_id, kMaximumSecretBytes);
}

base::Result<contracts::SecretRef>
protect_local_machine_secret_blob(const std::string_view secret_material,
                                  const std::string_view entropy_id) {
    return protect_secret_with_limit(secret_material, entropy_id, kMaximumSecretBlobBytes);
}

} // namespace aegra::adapters::windows_system
