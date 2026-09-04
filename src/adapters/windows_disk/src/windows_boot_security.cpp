#include "windows_boot_security.h"

#include <Wbemidl.h>
#include <combaseapi.h>
#include <oleauto.h>

#include <cwchar>
#include <filesystem>
#include <utility>

namespace aegra::adapters::windows_disk::detail {
namespace {

template <typename Interface> class ComPtr final {
  public:
    ComPtr() noexcept = default;
    ~ComPtr() { reset(); }
    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;
    ComPtr(ComPtr&& other) noexcept : value_(std::exchange(other.value_, nullptr)) {}
    ComPtr& operator=(ComPtr&& other) noexcept {
        if (this != &other) {
            reset(std::exchange(other.value_, nullptr));
        }
        return *this;
    }

    [[nodiscard]] Interface* get() const noexcept { return value_; }
    [[nodiscard]] Interface** put() noexcept {
        reset();
        return &value_;
    }
    void reset(Interface* value = nullptr) noexcept {
        if (value_ != nullptr) {
            value_->Release();
        }
        value_ = value;
    }

  private:
    Interface* value_{nullptr};
};

class ScopedComApartment final {
  public:
    ScopedComApartment() noexcept : result_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ScopedComApartment() {
        if (result_ == S_OK || result_ == S_FALSE) {
            CoUninitialize();
        }
    }
    ScopedComApartment(const ScopedComApartment&) = delete;
    ScopedComApartment& operator=(const ScopedComApartment&) = delete;
    [[nodiscard]] bool available() const noexcept {
        return SUCCEEDED(result_) || result_ == RPC_E_CHANGED_MODE;
    }

  private:
    HRESULT result_;
};

[[nodiscard]] bool same_volume_path(std::wstring left, std::wstring right) noexcept {
    while (!left.empty() && (left.back() == L'\\' || left.back() == L'/')) {
        left.pop_back();
    }
    while (!right.empty() && (right.back() == L'\\' || right.back() == L'/')) {
        right.pop_back();
    }
    return _wcsicmp(left.c_str(), right.c_str()) == 0;
}

[[nodiscard]] WindowsSecurityState protection_state(const VARIANT& value) noexcept {
    if (value.vt == VT_I4) {
        return value.lVal == 1 ? WindowsSecurityState::kEnabled
                              : value.lVal == 0 ? WindowsSecurityState::kDisabled
                                                : WindowsSecurityState::kUnknown;
    }
    if (value.vt == VT_UI4) {
        return value.ulVal == 1 ? WindowsSecurityState::kEnabled
                               : value.ulVal == 0 ? WindowsSecurityState::kDisabled
                                                 : WindowsSecurityState::kUnknown;
    }
    return WindowsSecurityState::kUnknown;
}

[[nodiscard]] WindowsSecurityState find_volume(IEnumWbemClassObject& rows,
                                               const std::filesystem::path& volume_path) noexcept {
    constexpr ULONGLONG kQueryTimeoutMilliseconds = 5000;
    const auto deadline = GetTickCount64() + kQueryTimeoutMilliseconds;
    for (;;) {
        const auto now = GetTickCount64();
        if (now >= deadline) {
            return WindowsSecurityState::kUnknown;
        }
        ComPtr<IWbemClassObject> row;
        ULONG returned = 0;
        const auto next = rows.Next(static_cast<LONG>(deadline - now), 1, row.put(), &returned);
        if (FAILED(next) || returned == 0) {
            return WindowsSecurityState::kUnknown;
        }
        VARIANT device_id{};
        VARIANT status{};
        const auto device_result = row.get()->Get(L"DeviceID", 0, &device_id, nullptr, nullptr);
        const auto status_result =
            row.get()->Get(L"ProtectionStatus", 0, &status, nullptr, nullptr);
        const bool matches = SUCCEEDED(device_result) && device_id.vt == VT_BSTR &&
                             device_id.bstrVal != nullptr &&
                             same_volume_path(device_id.bstrVal, volume_path.wstring());
        const auto mapped = SUCCEEDED(status_result) ? protection_state(status)
                                                     : WindowsSecurityState::kUnknown;
        VariantClear(&status);
        VariantClear(&device_id);
        if (matches) {
            return mapped;
        }
    }
}

} // namespace

WindowsSecurityState
inspect_bitlocker_state(const std::filesystem::path& volume_guid_path) noexcept {
    ScopedComApartment apartment;
    if (!apartment.available()) {
        return WindowsSecurityState::kUnknown;
    }
    const auto security = CoInitializeSecurity(nullptr, -1, nullptr, nullptr,
                                               RPC_C_AUTHN_LEVEL_DEFAULT,
                                               RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE,
                                               nullptr);
    if (FAILED(security) && security != RPC_E_TOO_LATE) {
        return WindowsSecurityState::kUnknown;
    }

    ComPtr<IWbemLocator> locator;
    if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER,
                                IID_IWbemLocator, reinterpret_cast<void**>(locator.put())))) {
        return WindowsSecurityState::kUnknown;
    }
    ComPtr<IWbemServices> services;
    BSTR namespace_path = SysAllocString(L"ROOT\\CIMV2\\Security\\MicrosoftVolumeEncryption");
    if (namespace_path == nullptr) {
        return WindowsSecurityState::kUnknown;
    }
    const auto connected = locator.get()->ConnectServer(namespace_path, nullptr, nullptr, nullptr, 0,
                                                        nullptr, nullptr, services.put());
    SysFreeString(namespace_path);
    if (FAILED(connected) || services.get() == nullptr ||
        FAILED(CoSetProxyBlanket(services.get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr,
                                 RPC_C_AUTHN_LEVEL_CALL, RPC_C_IMP_LEVEL_IMPERSONATE, nullptr,
                                 EOAC_NONE))) {
        return WindowsSecurityState::kUnknown;
    }

    ComPtr<IEnumWbemClassObject> rows;
    BSTR language = SysAllocString(L"WQL");
    BSTR query = SysAllocString(
        L"SELECT DeviceID, ProtectionStatus FROM Win32_EncryptableVolume");
    if (language == nullptr || query == nullptr) {
        SysFreeString(query);
        SysFreeString(language);
        return WindowsSecurityState::kUnknown;
    }
    const auto executed = services.get()->ExecQuery(
        language, query, WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, rows.put());
    SysFreeString(query);
    SysFreeString(language);
    if (FAILED(executed) || rows.get() == nullptr) {
        return WindowsSecurityState::kUnknown;
    }
    return find_volume(*rows.get(), volume_guid_path);
}

} // namespace aegra::adapters::windows_disk::detail
