# Aegra Explorer Shell Extension — COM surface from vendored MSF (third_party/msf).
# MSF provides IShellFolder/IEnumIDList/PIDL/registration templates (header-only ATL).
# Archive browse logic is Aegra ArchiveShellModel (not msf_host / backup engine).

add_library(aegra_shell_extension SHARED
    src/pch.cpp
    src/main.cpp
    src/host_shell_folder.cpp
    src/host_item.cpp
    src/archive_shell_model.cpp
    src/archive_chain_resolver.cpp
    src/password_dialog.cpp
    src/protected_password.cpp
    src/secure_password.cpp
    src/shell_debug_log.cpp
    src/shell_strings.cpp
    resources/aegra_shell_extension.rc
    resources/aegra_shell_extension.def
)

add_library(Aegra::ShellExtension ALIAS aegra_shell_extension)

target_precompile_headers(
    aegra_shell_extension
    PRIVATE
        src/pch.h
)

target_compile_definitions(
    aegra_shell_extension
    PRIVATE
        UNICODE
        _UNICODE
        NOMINMAX
        STRICT_TYPED_ITEMIDS
        _ATL_APARTMENT_THREADED
        _CRT_SECURE_NO_WARNINGS
        ISOLATION_AWARE_ENABLED=1
)

target_include_directories(
    aegra_shell_extension
    PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/resources>"
)

target_link_libraries(
    aegra_shell_extension
    PRIVATE
        Aegra::ThirdPartyMsf
        Aegra::Base
        Aegra::Contracts
        Aegra::Ports
        Aegra::Format
        Aegra::PersonalRepository
        Aegra::AdapterStorageLocal
        Aegra::AdapterPersonalArchive
        Aegra::AdapterNtfs
        ole32
        oleaut32
        uuid
        shell32
        shlwapi
        advapi32
        crypt32
        user32
        comctl32
        gdi32
        atls
)

set_target_properties(
    aegra_shell_extension
    PROPERTIES
        OUTPUT_NAME "aegra_shell_extension"
        PREFIX ""
        WINDOWS_EXPORT_ALL_SYMBOLS FALSE
)

if(MSVC)
    # MSF/ATL templates are large; avoid C1128.
    target_compile_options(aegra_shell_extension PRIVATE /bigobj)
    get_target_property(_shell_opts aegra_shell_extension COMPILE_OPTIONS)
    if(_shell_opts)
        list(REMOVE_ITEM _shell_opts /WX)
        set_property(TARGET aegra_shell_extension PROPERTY COMPILE_OPTIONS "${_shell_opts}")
    endif()
endif()

aegra_configure_target(aegra_shell_extension)

if(MSVC AND AEGRA_WARNINGS_AS_ERRORS)
    # Keep /W4 from aegra_configure_target but do not fail the MSF-based host on /WX.
    target_compile_options(aegra_shell_extension PRIVATE /WX-)
endif()
