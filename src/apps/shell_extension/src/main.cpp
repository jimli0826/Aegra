#include "pch.h"

#include "host_shell_folder_class_id.h"
#include "resource.h"

class Module final : public ATL::CAtlDllModuleT<Module> {};

MSF_WARNING_SUPPRESS(26426)
Module _Module;
MSF_WARNING_UNSUPPRESS()

extern "C" BOOL __stdcall DllMain(HINSTANCE hInstance, DWORD dwReason, LPVOID lpReserved) {
    if (!_Module.DllMain(dwReason, lpReserved)) {
        return FALSE;
    }
    msf::IsolationAwareDllMain(dwReason);
    if (dwReason == DLL_PROCESS_ATTACH) {
        ATLVERIFY(DisableThreadLibraryCalls(hInstance));
    }
    return TRUE;
}

__control_entrypoint(DllExport)
STDAPI DllCanUnloadNow() {
    return _Module.DllCanUnloadNow();
}

_Check_return_
STDAPI DllGetClassObject(_In_ REFCLSID classId, _In_ REFIID interfaceId, _Outptr_ LPVOID* ppv) {
    return _Module.DllGetClassObject(classId, interfaceId, ppv);
}

#if WDK_NTDDI_VERSION < 0x0A000007
__control_entrypoint(DllExport)
#endif
STDAPI DllRegisterServer() {
    auto hr = _Module.DllRegisterServer(false);
    if (FAILED(hr)) {
        return hr;
    }
    hr = msf::UpdateRegistryConnectExtensionToProgId(IDR_EXTENSION, TRUE,
                                                     aegra::shell::kFileExtension,
                                                     aegra::shell::kFileRootProgId);
    if (FAILED(hr)) {
        return hr;
    }
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}

#if WDK_NTDDI_VERSION < 0x0A000007
__control_entrypoint(DllExport)
#endif
STDAPI DllUnregisterServer() {
    _Module.DllUnregisterServer();
    std::ignore = msf::UpdateRegistryConnectExtensionToProgId(
        IDR_EXTENSION, FALSE, aegra::shell::kFileExtension, aegra::shell::kFileRootProgId);
    SHChangeNotify(SHCNE_ASSOCCHANGED, SHCNF_IDLIST, nullptr, nullptr);
    return S_OK;
}
