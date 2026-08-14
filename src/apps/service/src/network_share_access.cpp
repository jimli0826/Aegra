#include "network_share_access.h"

#include "aegra/adapters/windows_system/windows_system.h"

#include <WinSock2.h>
#include <Windows.h>
#include <Ws2tcpip.h>
#include <winnetwk.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstring>
#include <string>
#include <utility>

namespace aegra::apps::service {
namespace {

constexpr std::string_view kNetworkAuthMagic = "aegra-net1\n";
constexpr u_short kSmbPort = 445;

[[nodiscard]] base::Error make_error(const base::ErrorCode code, const char* message) {
    return {code, message};
}

[[nodiscard]] base::Error network_connection_error(const DWORD status) {
    switch (status) {
    case ERROR_LOGON_FAILURE:
    case ERROR_INVALID_PASSWORD:
    case ERROR_BAD_USERNAME:
    case ERROR_ACCOUNT_RESTRICTION:
    case ERROR_ACCOUNT_DISABLED:
    case ERROR_PASSWORD_EXPIRED:
    case ERROR_LOGON_TYPE_NOT_GRANTED:
        return make_error(base::ErrorCode::kUnauthorized,
                          "repository.network_credentials_rejected");
    case ERROR_ACCESS_DENIED:
        return make_error(base::ErrorCode::kUnauthorized, "repository.network_access_denied");
    case ERROR_BAD_NET_NAME:
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return make_error(base::ErrorCode::kNotFound, "repository.network_share_not_found");
    case ERROR_BAD_NETPATH:
    case ERROR_NETWORK_UNREACHABLE:
    case ERROR_HOST_UNREACHABLE:
    case ERROR_NO_NETWORK:
    case ERROR_CONNECTION_UNAVAIL:
    case ERROR_SEM_TIMEOUT:
    case ERROR_UNEXP_NET_ERR:
        return make_error(base::ErrorCode::kIoFailure, "repository.network_unreachable");
    case ERROR_SESSION_CREDENTIAL_CONFLICT:
        return make_error(base::ErrorCode::kConflict, "repository.network_credential_conflict");
    default:
        return make_error(base::ErrorCode::kIoFailure, "repository.network_connect_failed");
    }
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
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()),
                        wide.data(), needed);
    return wide;
}

[[nodiscard]] std::string wide_to_utf8(const std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                           nullptr, 0, nullptr, nullptr);
    if (needed <= 0) {
        return {};
    }
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    if (WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), utf8.data(),
                            needed, nullptr, nullptr) != needed) {
        return {};
    }
    return utf8;
}

class FindHandle final {
  public:
    explicit FindHandle(const HANDLE value) noexcept : value_(value) {}
    ~FindHandle() {
        if (value_ != INVALID_HANDLE_VALUE) {
            FindClose(value_);
        }
    }
    FindHandle(const FindHandle&) = delete;
    FindHandle& operator=(const FindHandle&) = delete;
    [[nodiscard]] HANDLE get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE; }

  private:
    HANDLE value_{INVALID_HANDLE_VALUE};
};

[[nodiscard]] bool is_directory_entry(const WIN32_FIND_DATAW& data) noexcept {
    const std::wstring_view name(data.cFileName);
    return name != L"." && name != L".." &&
           (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
           (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0;
}

[[nodiscard]] base::Result<contracts::FileSourceNodePage>
enumerate_directories(const std::string_view locator, const contracts::ServicePageRequest& page,
                      const base::CancellationToken cancellation) {
    auto root = utf8_to_wide(locator);
    if (root.empty()) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"));
    }
    while (!root.empty() && (root.back() == L'\\' || root.back() == L'/')) {
        root.pop_back();
    }
    const auto pattern = root + L"\\*";
    WIN32_FIND_DATAW data{};
    FindHandle find(FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &data, FindExSearchNameMatch,
                                     nullptr, 0));
    if (!find.valid()) {
        return base::Result<contracts::FileSourceNodePage>::failure(
            network_connection_error(GetLastError()));
    }
    std::uint64_t offset = 0;
    if (page.continuation_token) {
        const auto& value = *page.continuation_token;
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), offset);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
            return base::Result<contracts::FileSourceNodePage>::failure(
                make_error(base::ErrorCode::kInvalidArgument, "repository.directory_page_invalid"));
        }
    }
    contracts::FileSourceNodePage result;
    std::uint64_t directory_index = 0;
    do {
        if (cancellation.stop_requested()) {
            return base::Result<contracts::FileSourceNodePage>::failure(
                make_error(base::ErrorCode::kCancelled, "repository.directory_list_cancelled"));
        }
        if (!is_directory_entry(data)) {
            continue;
        }
        if (directory_index++ < offset) {
            continue;
        }
        if (result.items.size() >= page.maximum_results) {
            result.continuation_token = std::to_string(directory_index - 1U);
            break;
        }
        auto display_name = wide_to_utf8(data.cFileName);
        if (display_name.empty()) {
            continue;
        }
        contracts::FileSourceNode node;
        node.node_token = "repository-directory-" + std::to_string(directory_index);
        node.display_name = std::move(display_name);
        node.entry_kind = contracts::FileEntryKind::kDirectory;
        node.selectability = contracts::FileNodeSelectability::kSelectable;
        node.has_children = true;
        node.is_directory = true;
        node.availability = contracts::SourceAvailability::kAvailable;
        result.items.push_back(std::move(node));
    } while (FindNextFileW(find.get(), &data) != FALSE);
    return base::Result<contracts::FileSourceNodePage>::success(std::move(result));
}

class WinsockSession final {
  public:
    WinsockSession() noexcept { ready_ = WSAStartup(MAKEWORD(2, 2), &data_) == 0; }
    ~WinsockSession() {
        if (ready_) {
            WSACleanup();
        }
    }
    WinsockSession(const WinsockSession&) = delete;
    WinsockSession& operator=(const WinsockSession&) = delete;

    [[nodiscard]] bool ready() const noexcept { return ready_; }

  private:
    WSADATA data_{};
    bool ready_{false};
};

class SocketHandle final {
  public:
    explicit SocketHandle(const SOCKET value) noexcept : value_(value) {}
    ~SocketHandle() {
        if (value_ != INVALID_SOCKET) {
            closesocket(value_);
        }
    }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;

    [[nodiscard]] SOCKET get() const noexcept { return value_; }
    [[nodiscard]] bool valid() const noexcept { return value_ != INVALID_SOCKET; }

  private:
    SOCKET value_{INVALID_SOCKET};
};

[[nodiscard]] std::string extract_server_name(const std::string_view locator) {
    if (!is_unc_locator(locator)) {
        return {};
    }
    const auto server_end = locator.find('\\', 2);
    return server_end == std::string_view::npos || server_end == 2
               ? std::string{}
               : std::string(locator.substr(2, server_end - 2));
}

[[nodiscard]] bool parse_ip_literal(const std::wstring_view server, SOCKADDR_STORAGE& address,
                                    int& address_size) {
    auto* ipv4 = reinterpret_cast<SOCKADDR_IN*>(&address);
    if (InetPtonW(AF_INET, server.data(), &ipv4->sin_addr) == 1) {
        ipv4->sin_family = AF_INET;
        ipv4->sin_port = htons(kSmbPort);
        address_size = sizeof(*ipv4);
        return true;
    }
    auto* ipv6 = reinterpret_cast<SOCKADDR_IN6*>(&address);
    if (InetPtonW(AF_INET6, server.data(), &ipv6->sin6_addr) == 1) {
        ipv6->sin6_family = AF_INET6;
        ipv6->sin6_port = htons(kSmbPort);
        address_size = sizeof(*ipv6);
        return true;
    }
    return false;
}

[[nodiscard]] base::Result<void> wait_for_connection(const SOCKET socket,
                                                     const std::chrono::milliseconds timeout,
                                                     const base::CancellationToken cancellation) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (cancellation.stop_requested()) {
            return base::Result<void>::failure(
                make_error(base::ErrorCode::kCancelled, "repository network probe cancelled"));
        }
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        const auto slice = (std::min)(remaining, std::chrono::milliseconds(100));
        TIMEVAL wait{0, static_cast<long>(slice.count() * 1000)};
        fd_set writable;
        FD_ZERO(&writable);
        FD_SET(socket, &writable);
        const int selected = select(0, nullptr, &writable, nullptr, &wait);
        if (selected == SOCKET_ERROR) {
            break;
        }
        if (selected > 0) {
            int socket_error = 0;
            int size = sizeof(socket_error);
            if (getsockopt(socket, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&socket_error),
                           &size) == 0 &&
                socket_error == 0) {
                return base::Result<void>::success();
            }
            break;
        }
    }
    return base::Result<void>::failure(
        make_error(base::ErrorCode::kIoFailure, "repository.network_unreachable"));
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
    if (server_end == std::string_view::npos || server_end == 2 ||
        server_end + 1 >= locator.size()) {
        return {};
    }
    const auto share_end = locator.find('\\', server_end + 1);
    if (share_end == server_end + 1) {
        return {};
    }
    if (share_end == std::string_view::npos) {
        return std::string(locator);
    }
    return std::string(locator.substr(0, share_end));
}

base::Result<void> probe_network_share_server(const std::string_view locator,
                                              const base::CancellationToken cancellation,
                                              const std::chrono::milliseconds timeout) {
    if (!is_unc_locator(locator)) {
        return base::Result<void>::success();
    }
    if (timeout.count() <= 0) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "repository network timeout invalid"));
    }
    const auto server = utf8_to_wide(extract_server_name(locator));
    WinsockSession winsock;
    if (!winsock.ready()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kIoFailure, "repository network probe unavailable"));
    }
    SOCKADDR_STORAGE address{};
    int address_size = 0;
    if (server.empty() || !parse_ip_literal(server, address, address_size)) {
        return base::Result<void>::success();
    }
    SocketHandle socket(::socket(address.ss_family, SOCK_STREAM, IPPROTO_TCP));
    if (!socket.valid()) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kIoFailure, "repository network probe unavailable"));
    }
    u_long nonblocking = 1;
    if (ioctlsocket(socket.get(), FIONBIO, &nonblocking) != 0) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kIoFailure, "repository network probe unavailable"));
    }
    if (connect(socket.get(), reinterpret_cast<const SOCKADDR*>(&address), address_size) == 0) {
        return base::Result<void>::success();
    }
    if (WSAGetLastError() != WSAEWOULDBLOCK) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kIoFailure, "repository.network_unreachable"));
    }
    return wait_for_connection(socket.get(), timeout, cancellation);
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

    const DWORD status = try_connect();
    if (status != NO_ERROR && status != ERROR_ALREADY_ASSIGNED &&
        status != ERROR_DEVICE_ALREADY_REMEMBERED) {
        return base::Result<void>::failure(network_connection_error(status));
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

base::Result<void> connect_network_share_from_secret(const std::string_view locator,
                                                     const contracts::SecretRef& secret_ref) {
    auto resolved = adapters::windows_system::WindowsCredentialResolver{}.resolve(secret_ref, {});
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

base::Result<void> RepositoryLocationBrowseRegistry::register_location(
    const std::string_view session_id, std::string location_token, std::string locator) {
    if (session_id.empty() || location_token.empty() || !is_unc_locator(locator)) {
        return base::Result<void>::failure(
            make_error(base::ErrorCode::kInvalidArgument, "repository.network_path_invalid"));
    }
    std::lock_guard lock(mutex_);
    for (auto iterator = locations_.begin(); iterator != locations_.end();) {
        if (iterator->second.session_id == session_id) {
            iterator = locations_.erase(iterator);
        } else {
            ++iterator;
        }
    }
    locations_.insert_or_assign(location_token,
                                Location{std::string(session_id), std::move(locator)});
    return base::Result<void>::success();
}

base::Result<contracts::FileSourceNodePage> RepositoryLocationBrowseRegistry::list_directories(
    const std::string_view session_id, const contracts::RepositoryDirectoryListRequest& request,
    const base::CancellationToken cancellation) {
    std::string locator;
    {
        std::lock_guard lock(mutex_);
        const auto found = locations_.find(request.location_token);
        if (found == locations_.end() || found->second.session_id != session_id) {
            return base::Result<contracts::FileSourceNodePage>::failure(
                make_error(base::ErrorCode::kNotFound, "repository.directory_token_invalid"));
        }
        locator = found->second.locator;
    }
    return enumerate_directories(locator, request.page, cancellation);
}

void RepositoryLocationBrowseRegistry::clear_session(const std::string_view session_id) noexcept {
    std::lock_guard lock(mutex_);
    for (auto iterator = locations_.begin(); iterator != locations_.end();) {
        if (iterator->second.session_id == session_id) {
            iterator = locations_.erase(iterator);
        } else {
            ++iterator;
        }
    }
}

} // namespace aegra::apps::service
