#include "aegra/adapters/windows_ipc/windows_named_pipe_security.h"

#include "windows_named_pipe_security_internal.h"

#include <Windows.h>
#include <sddl.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace aegra::adapters::windows_ipc {
namespace {

class UniqueHandle final {
  public:
    explicit UniqueHandle(const HANDLE handle = nullptr) noexcept : handle_(handle) {}
    ~UniqueHandle() {
        if (valid()) {
            CloseHandle(handle_);
        }
    }

    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    UniqueHandle(UniqueHandle&& other) noexcept : handle_(other.release()) {}
    UniqueHandle& operator=(UniqueHandle&& other) noexcept {
        if (this != &other) {
            if (valid()) {
                CloseHandle(handle_);
            }
            handle_ = other.release();
        }
        return *this;
    }

    [[nodiscard]] HANDLE get() const noexcept { return handle_; }
    [[nodiscard]] bool valid() const noexcept {
        return handle_ != nullptr && handle_ != INVALID_HANDLE_VALUE;
    }
    [[nodiscard]] HANDLE release() noexcept { return std::exchange(handle_, nullptr); }

  private:
    HANDLE handle_{nullptr};
};

[[nodiscard]] base::Result<std::string> sid_to_sddl(const PSID sid) {
    LPWSTR text = nullptr;
    if (ConvertSidToStringSidW(sid, &text) == FALSE) {
        return base::Result<std::string>::failure(
            base::Error{base::ErrorCode::kInternal, "named pipe peer SID conversion failed"});
    }
    detail::UniqueLocal owned(text);
    std::string result;
    for (const wchar_t* cursor = text; *cursor != L'\0'; ++cursor) {
        if (*cursor > 0x7F) {
            return base::Result<std::string>::failure(
                base::Error{base::ErrorCode::kInternal, "named pipe peer SID is not ASCII"});
        }
        result.push_back(static_cast<char>(*cursor));
    }
    return base::Result<std::string>::success(std::move(result));
}

[[nodiscard]] bool token_has_well_known_sid(const HANDLE token, const WELL_KNOWN_SID_TYPE type) {
    std::array<std::byte, SECURITY_MAX_SID_SIZE> buffer{};
    DWORD size = static_cast<DWORD>(buffer.size());
    auto* sid = reinterpret_cast<PSID>(buffer.data());
    if (CreateWellKnownSid(type, nullptr, sid, &size) == FALSE) {
        return false;
    }
    BOOL is_member = FALSE;
    return CheckTokenMembership(token, sid, &is_member) != FALSE && is_member != FALSE;
}

[[nodiscard]] base::Result<WindowsNamedPipePeerIdentity>
identity_from_token(const HANDLE token, const std::uint32_t process_id,
                    const std::uint32_t session_id) {
    DWORD required = 0;
    (void)GetTokenInformation(token, TokenUser, nullptr, 0, &required);
    if (required == 0) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer token is unavailable"});
    }
    std::vector<std::byte> storage(required);
    if (GetTokenInformation(token, TokenUser, storage.data(), required, &required) == FALSE) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer token is unavailable"});
    }
    const auto* user = reinterpret_cast<const TOKEN_USER*>(storage.data());
    auto sid = sid_to_sddl(user->User.Sid);
    if (!sid) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(sid.error());
    }
    WindowsNamedPipePeerIdentity identity;
    identity.user_sid = std::move(sid).value();
    identity.session_id = session_id;
    identity.process_id = process_id;
    identity.is_local = true;
    identity.is_administrator = token_has_well_known_sid(token, WinBuiltinAdministratorsSid);
    // Session 0 is the service session. Personal Desktop clients use interactive sessions (>0).
    // Elevated admin tokens still qualify even when the Interactive group is filtered.
    identity.is_interactive =
        session_id > 0 || token_has_well_known_sid(token, WinInteractiveSid) ||
        identity.is_administrator;
    return base::Result<WindowsNamedPipePeerIdentity>::success(std::move(identity));
}

} // namespace

namespace detail {

base::Result<SECURITY_ATTRIBUTES*>
create_service_local_security_attributes(SECURITY_ATTRIBUTES& attributes,
                                         UniqueLocal& descriptor_owner) {
    // LocalSystem + Administrators full; Interactive users read/write. No network ACE.
    constexpr auto kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)";
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (ConvertStringSecurityDescriptorToSecurityDescriptorW(kSddl, SDDL_REVISION_1, &descriptor,
                                                             nullptr) == FALSE) {
        return base::Result<SECURITY_ATTRIBUTES*>::failure(base::Error{
            base::ErrorCode::kInternal, "named pipe security descriptor creation failed"});
    }
    descriptor_owner = UniqueLocal(descriptor);
    attributes = {};
    attributes.nLength = sizeof(attributes);
    attributes.lpSecurityDescriptor = descriptor;
    attributes.bInheritHandle = FALSE;
    return base::Result<SECURITY_ATTRIBUTES*>::success(&attributes);
}

base::Result<WindowsNamedPipePeerIdentity> query_pipe_peer(const HANDLE pipe) {
    if (pipe == nullptr || pipe == INVALID_HANDLE_VALUE) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kInvalidArgument, "named pipe handle is invalid"});
    }
    ULONG process_id = 0;
    ULONG session_id = 0;
    if (GetNamedPipeClientProcessId(pipe, &process_id) == FALSE ||
        GetNamedPipeClientSessionId(pipe, &session_id) == FALSE || process_id == 0) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer process is unavailable"});
    }
    // Prefer the client process token over ImpersonateNamedPipeClient so identity checks do not
    // depend on CreateFile SQOS impersonation level or pipe ACE privileges.
    UniqueHandle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, process_id));
    if (!process.valid()) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer process open failed"});
    }
    HANDLE raw_token = nullptr;
    if (OpenProcessToken(process.get(), TOKEN_QUERY, &raw_token) == FALSE) {
        return base::Result<WindowsNamedPipePeerIdentity>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer token open failed"});
    }
    UniqueHandle token(raw_token);
    return identity_from_token(token.get(), process_id, session_id);
}

} // namespace detail

base::Result<void>
authorize_service_caller(const WindowsNamedPipePeerIdentity& peer,
                         const WindowsServiceCallerAuthorization& authorization) {
    if (peer.user_sid.empty() || !peer.is_local || peer.process_id == 0) {
        return base::Result<void>::failure(
            base::Error{base::ErrorCode::kUnauthorized, "named pipe peer identity is incomplete"});
    }
    if (authorization.policy != WindowsServiceCallerPolicy::kLocalInteractiveOrAdmin) {
        return base::Result<void>::failure(base::Error{base::ErrorCode::kInvalidArgument,
                                                       "named pipe authorization policy is invalid"});
    }
    if (peer.is_interactive || peer.is_administrator) {
        return base::Result<void>::success();
    }
    return base::Result<void>::failure(base::Error{
        base::ErrorCode::kUnauthorized, "named pipe peer is not an authorized interactive user"});
}

} // namespace aegra::adapters::windows_ipc
