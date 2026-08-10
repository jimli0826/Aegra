#include "windows_cng_sha256.h"

#include "aegra/base/error.h"

#include <Windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace aegra::adapters::personal_archive::detail {
namespace {

[[nodiscard]] base::Error hash_error(const char* message) {
    return base::Error{base::ErrorCode::kInternal, message};
}

[[nodiscard]] PUCHAR as_unsigned(std::byte* value) noexcept {
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) CNG byte-buffer boundary.
    return reinterpret_cast<PUCHAR>(value);
}

[[nodiscard]] PUCHAR as_unsigned(const std::byte* value) noexcept {
    // BCryptHashData does not mutate input but its legacy signature is not const-correct.
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-reinterpret-cast) CNG byte-buffer boundary.
    return reinterpret_cast<PUCHAR>(const_cast<std::byte*>(value));
}

} // namespace

struct WindowsCngSha256::Impl final {
    ~Impl() {
        if (hash != nullptr) {
            BCryptDestroyHash(hash);
        }
        if (algorithm != nullptr) {
            BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    }

    BCRYPT_ALG_HANDLE algorithm{nullptr};
    BCRYPT_HASH_HANDLE hash{nullptr};
    std::vector<std::byte> hash_object;
    base::Error initialization_error{};
};

WindowsCngSha256::WindowsCngSha256() : impl_(std::make_unique<Impl>()) {
    auto status =
        BCryptOpenAlgorithmProvider(&impl_->algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    DWORD object_size = 0;
    DWORD copied = 0;
    if (BCRYPT_SUCCESS(status)) {
        status = BCryptGetProperty(impl_->algorithm, BCRYPT_OBJECT_LENGTH,
                                   reinterpret_cast<PUCHAR>(&object_size), sizeof(object_size),
                                   &copied, 0);
    }
    if (BCRYPT_SUCCESS(status) && copied == sizeof(object_size) && object_size != 0) {
        impl_->hash_object.resize(object_size);
        status = BCryptCreateHash(impl_->algorithm, &impl_->hash,
                                  as_unsigned(impl_->hash_object.data()), object_size, nullptr, 0,
                                  BCRYPT_HASH_REUSABLE_FLAG);
    }
    if (!BCRYPT_SUCCESS(status) || impl_->hash == nullptr) {
        impl_->initialization_error = hash_error("Windows CNG SHA-256 initialization failed");
    }
}

WindowsCngSha256::~WindowsCngSha256() = default;
WindowsCngSha256::WindowsCngSha256(WindowsCngSha256&&) noexcept = default;
WindowsCngSha256& WindowsCngSha256::operator=(WindowsCngSha256&&) noexcept = default;

base::Result<CngSha256Digest> WindowsCngSha256::hash(const std::span<const std::byte> input) {
    if (impl_->initialization_error.is_error()) {
        return base::Result<CngSha256Digest>::failure(impl_->initialization_error);
    }
    std::size_t consumed = 0;
    while (consumed < input.size()) {
        const auto remaining = input.size() - consumed;
        const auto quantum = (std::min)(
            remaining, static_cast<std::size_t>((std::numeric_limits<ULONG>::max)()));
        const auto status = BCryptHashData(impl_->hash, as_unsigned(input.data() + consumed),
                                           static_cast<ULONG>(quantum), 0);
        if (!BCRYPT_SUCCESS(status)) {
            return base::Result<CngSha256Digest>::failure(
                hash_error("Windows CNG SHA-256 update failed"));
        }
        consumed += quantum;
    }
    CngSha256Digest digest{};
    const auto status =
        BCryptFinishHash(impl_->hash, as_unsigned(digest.data()),
                         static_cast<ULONG>(digest.size()), 0);
    if (!BCRYPT_SUCCESS(status)) {
        return base::Result<CngSha256Digest>::failure(
            hash_error("Windows CNG SHA-256 finish failed"));
    }
    return base::Result<CngSha256Digest>::success(digest);
}

} // namespace aegra::adapters::personal_archive::detail
