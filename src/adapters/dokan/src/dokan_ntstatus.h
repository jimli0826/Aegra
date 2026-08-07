// Minimal NTSTATUS provisioning for dokan.h.
//
// ntstatus.h provides STATUS_* constants only when WIN32_NO_STATUS is unset, and
// does not typedef NTSTATUS itself. Provide the type (as the old Dokan stack did)
// then pull in status constants before including dokan.
#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

// Suppress win32 status macros so <ntstatus.h> can define the full STATUS_* set.
#define WIN32_NO_STATUS
#include <windows.h>
#undef WIN32_NO_STATUS

// NTSTATUS is typed in winternl/bcrypt, not in ntstatus.h.
#ifndef DOKAN_NTSTATUS_TYPEDEF_DEFINED
#define DOKAN_NTSTATUS_TYPEDEF_DEFINED
typedef LONG NTSTATUS;
typedef NTSTATUS* PNTSTATUS;
#endif

// Must not define _NTSTATUS_ before this include — that guard skips STATUS_*.
#include <ntstatus.h>

#include <dokan/dokan.h>
