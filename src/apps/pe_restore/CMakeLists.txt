find_package(nlohmann_json CONFIG REQUIRED)

add_executable(aegra_pe_restore WIN32
    src/main.cpp
    src/pe_run.cpp
    src/pe_strings.cpp
    src/pe_ui.cpp
    src/pe_worker_client.cpp
)

target_compile_definitions(
    aegra_pe_restore
    PRIVATE
        AEGRA_PE_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_link_libraries(
    aegra_pe_restore
    PRIVATE
        Aegra::Base
        Aegra::Contracts
        Aegra::Ports
        Aegra::AdapterCryptoSodium
        Aegra::AdapterWindowsDisk
        Aegra::AdapterWindowsIpc
        Aegra::AdapterWindowsPe
        Aegra::AdapterWindowsProcess
        Aegra::AdapterWindowsSystem
        nlohmann_json::nlohmann_json
)
aegra_configure_target(aegra_pe_restore)
aegra_add_version_resource(aegra_pe_restore "Aegra Image WinPE Restore")

# The service collects the WinPE image payload from its own directory
# (PeRestoreEnvironment.payload_directory_utf8), same as it resolves the worker.
if(TARGET aegra_service)
    add_dependencies(aegra_service aegra_pe_restore)
    add_custom_command(
        TARGET aegra_pe_restore
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:aegra_pe_restore>"
                "$<TARGET_FILE_DIR:aegra_service>/$<TARGET_FILE_NAME:aegra_pe_restore>"
        COMMENT "Copy aegra_pe_restore beside aegra_service"
    )
    # WinPE does not ship the MSVC runtime (vcruntime140 / msvcp140 /
    # msvcp140_atomic_wait / ...), and a missing import DLL fails silently there
    # (hard-error popups are suppressed). Stage the Release redist beside the
    # service; the payload collector requires every listed DLL and never injects
    # debug CRT.
    if(MSVC)
        set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
        include(InstallRequiredSystemLibraries)
        foreach(aegra_runtime_dll IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
            add_custom_command(
                TARGET aegra_pe_restore
                POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${aegra_runtime_dll}"
                        "$<TARGET_FILE_DIR:aegra_service>/"
                COMMENT "Stage MSVC runtime beside aegra_service for the PE payload"
            )
        endforeach()
    endif()
endif()
