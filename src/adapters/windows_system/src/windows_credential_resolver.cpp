#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>
#include <wincred.h>

#include <cstddef>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace aegra::adapters::windows_system {
namespace {

constexpr std::string_view kCredentialPrefix = "wincred://";

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

class UniqueCredential final {
  public:
    explicit UniqueCredential(PCREDENTIALW credential) noexcept : credential_(credential) {}
    ~UniqueCredential() {
        if (credential_ != nullptr) {
            CredFree(credential_);
        }
    }

    UniqueCredential(const UniqueCredential&) = delete;
    UniqueCredential& operator=(const UniqueCredential&) = delete;
    UniqueCredential(UniqueCredential&&) = delete;
    UniqueCredential& operator=(UniqueCredential&&) = delete;

  private:
    PCREDENTIALW credential_{nullptr};
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

base::Result<std::wstring> to_wide(const std::string& value) {
    if (value.empty() || value.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH) {
        return base::Result<std::wstring>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential target is invalid"});
    }
    const auto input_size = static_cast<int>(value.size());
    const auto required =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, nullptr, 0);
    if (required == 0) {
        return base::Result<std::wstring>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential target is not UTF-8"});
    }
    std::wstring result(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), input_size, result.data(),
                            required) == 0) {
        return base::Result<std::wstring>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential target is not UTF-8"});
    }
    return base::Result<std::wstring>::success(std::move(result));
}

base::Error credential_error(const DWORD error) {
    const auto code =
        error == ERROR_NOT_FOUND ? base::ErrorCode::kNotFound : base::ErrorCode::kUnauthorized;
    return base::Error{code, "Windows credential lookup failed"};
}

} // namespace

base::Result<std::unique_ptr<ports::IResolvedSecret>>
WindowsCredentialResolver::resolve(const contracts::SecretRef& secret_ref,
                                   const base::CancellationToken& cancellation) {
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kCancelled, "credential resolution cancelled"});
    }
    if (!secret_ref.value.starts_with(kCredentialPrefix)) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential reference is unsupported"});
    }
    auto target = to_wide(secret_ref.value.substr(kCredentialPrefix.size()));
    if (!target) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(target.error());
    }
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.value().c_str(), CRED_TYPE_GENERIC, 0, &credential)) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            credential_error(GetLastError()));
    }
    const UniqueCredential credential_guard(credential);
    if (cancellation.stop_requested()) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kCancelled, "credential resolution cancelled"});
    }
    if (credential->CredentialBlobSize == 0 || credential->CredentialBlob == nullptr) {
        return base::Result<std::unique_ptr<ports::IResolvedSecret>>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "Windows credential is empty"});
    }
    const auto blob =
        std::span<const BYTE>(credential->CredentialBlob, credential->CredentialBlobSize);
    const auto bytes = std::as_bytes(blob);
    return LockedSecret::create(bytes);
}

base::Result<contracts::SecretRef>
store_generic_windows_credential(const std::string_view target_name,
                                 const std::string_view secret_material) {
    if (target_name.empty() || target_name.size() > CRED_MAX_GENERIC_TARGET_NAME_LENGTH ||
        secret_material.empty() || secret_material.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) {
        return base::Result<contracts::SecretRef>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "credential material is invalid"});
    }
    auto target = to_wide(std::string(target_name));
    if (!target) {
        return base::Result<contracts::SecretRef>::failure(target.error());
    }
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = target.value().data();
    credential.CredentialBlobSize = static_cast<DWORD>(secret_material.size());
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-const-cast) CredWriteW requires LPBYTE.
    credential.CredentialBlob =
        reinterpret_cast<LPBYTE>(const_cast<char*>(secret_material.data()));
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.UserName = nullptr;
    if (!CredWriteW(&credential, 0)) {
        return base::Result<contracts::SecretRef>::failure(
            base::Error{base::ErrorCode::kIoFailure, "Windows credential store failed"});
    }
    contracts::SecretRef reference;
    reference.value = std::string(kCredentialPrefix) + std::string(target_name);
    return base::Result<contracts::SecretRef>::success(std::move(reference));
}

} // namespace aegra::adapters::windows_system
