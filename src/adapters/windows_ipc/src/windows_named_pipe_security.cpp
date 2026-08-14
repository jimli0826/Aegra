#include "aegra/adapters/windows_ipc/windows_named_pipe_security.h"

#include "windows_named_pipe_security_internal.h"

#include <Windows.h>
#include <sddl.h>

namespace aegra::adapters::windows_ipc {
namespace detail {

base::Result<SECURITY_ATTRIBUTES*>
create_local_everyone_security_attributes(SECURITY_ATTRIBUTES& attributes,
                                          UniqueLocal& descriptor_owner) {
    // LocalSystem + Administrators full; all local users read/write. Remote clients are rejected
    // by the pipe mode rather than this DACL.
    constexpr auto kSddl = L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;WD)";
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

} // namespace detail

} // namespace aegra::adapters::windows_ipc
