#include "network_share_access.h"

#include "aegra/adapters/windows_system/windows_system.h"

#include <Windows.h>
#include <winnetwk.h>

#include <cstring>
#include <string>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kNetworkAuthMagic = "aegra-net1\n";

[[nodiscard]] base::Error make_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] std::wstring utf8_to_wide(const std::string_view value) {
    if (value.empty()) {
        return {};
    }
    const int needed = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                                           static_cast<int>(value.size()), nullptr, 0);
    if (needed <= 0) {
        return {};
    }
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
                        static_cast<int>(value.size()), wide.data(), needed);
    return wide;
}

} // namespace

bool is_unc_locator(const std::string_view locator) noexcept {
    return locator.size() > 3 && locator[0] == '\\' && locator[1] == '\\';
}

std::string extract_share_root(const std::string_view locator) {
    if (!is_unc_locator(locator)) {
        return {};
    }
    const auto server_end = locator.find('\\', 2);
    if (server_end == std::string_view::npos) {
        return {};
    }
    const auto share_end = locator.find('\\', server_end + 1);
    if (share_end == std::string_view::npos) {
        return std::string(locator);
    }
    return std::string(locator.substr(0, share_end));
}

base::Result<void> connect_network_share(const std::string_view locator,
                                         const std::string_view username,
                                         const std::string_view password,
                                         const std::string_view domain) {
    const auto share = extract_share_root(locator);
    if (share.empty()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"));
    }

    auto w_share = utf8_to_wide(share);
    if (w_share.empty() && !share.empty()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"));
    }

    std::string account(username);
    if (!domain.empty() && !account.empty() && account.find('\\') == std::string::npos &&
        account.find('@') == std::string::npos) {
        account = std::string(domain) + "\\" + account;
    }
    const auto w_user = utf8_to_wide(account);
    const auto w_pass = utf8_to_wide(password);

    NETRESOURCEW resource{};
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpRemoteName = w_share.data();

    auto try_connect = [&]() -> DWORD {
        return WNetAddConnection2W(&resource, w_pass.empty() ? nullptr : w_pass.c_str(),
                                   w_user.empty() ? nullptr : w_user.c_str(), CONNECT_TEMPORARY);
    };

    DWORD status = try_connect();
    if (status == ERROR_SESSION_CREDENTIAL_CONFLICT) {
        WNetCancelConnection2W(w_share.c_str(), 0, TRUE);
        status = try_connect();
    }
    if (status != NO_ERROR && status != ERROR_ALREADY_ASSIGNED &&
        status != ERROR_DEVICE_ALREADY_REMEMBERED) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kUnauthorized, "repository.network_connect_failed"));
    }
    return base::Result<void>::success();
}

std::string pack_network_auth_material(const std::string_view username,
                                       const std::string_view domain,
                                       const std::string_view password) {
    std::string packed;
    packed.reserve(kNetworkAuthMagic.size() + username.size() + domain.size() + password.size() +
                   3U);
    packed.append(kNetworkAuthMagic);
    packed.append(username);
    packed.push_back('\n');
    packed.append(domain);
    packed.push_back('\n');
    packed.append(password);
    return packed;
}

bool unpack_network_auth_material(const std::string_view material, std::string& username,
                                  std::string& domain, std::string& password) {
    if (!material.starts_with(kNetworkAuthMagic)) {
        return false;
    }
    const auto body = material.substr(kNetworkAuthMagic.size());
    const auto first = body.find('\n');
    if (first == std::string_view::npos) {
        return false;
    }
    const auto second = body.find('\n', first + 1);
    if (second == std::string_view::npos) {
        return false;
    }
    username.assign(body.substr(0, first));
    domain.assign(body.substr(first + 1, second - first - 1));
    password.assign(body.substr(second + 1));
    return true;
}

base::Result<void>
connect_network_share_from_secret(const std::string_view locator,
                                  const contracts::SecretRef& secret_ref) {
    auto resolved =
        adapters::windows_system::WindowsCredentialResolver{}.resolve(secret_ref, {});
    if (!resolved) {
        return base::Result<void>::failure(resolved.error());
    }
    const auto material = resolved.value()->view();
    std::string username;
    std::string domain;
    std::string password;
    if (!unpack_network_auth_material(material, username, domain, password)) {
        // Not a network pack (e.g. archive default credential) — no share connect needed.
        return base::Result<void>::success();
    }
    auto connected = connect_network_share(locator, username, password, domain);
    if (!password.empty()) {
        SecureZeroMemory(password.data(), password.size());
    }
    return connected;
}

} // namespace aegra::apps::service
