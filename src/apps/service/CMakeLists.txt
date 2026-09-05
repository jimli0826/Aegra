find_package(nlohmann_json CONFIG REQUIRED)

# Wire codec only: Service Host and AegraCLI share this library. It must not
# depend on Application, SQLite, Worker, or Windows data-plane adapters.
add_library(aegra_app_service_protocol STATIC
    src/service_protocol.cpp
    src/service_protocol_request_json.cpp
    src/service_protocol_response_json.cpp
)
add_library(Aegra::AppServiceProtocol ALIAS aegra_app_service_protocol)
target_include_directories(
    aegra_app_service_protocol
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)
target_link_libraries(
    aegra_app_service_protocol
    PUBLIC Aegra::Base Aegra::Contracts
    PRIVATE nlohmann_json::nlohmann_json
)
aegra_configure_target(aegra_app_service_protocol)

add_library(aegra_app_service STATIC
    src/backup_catalog_registrar.cpp
    src/file_recovery_chain.cpp
    src/file_recovery_point_query.cpp
    src/mount_supervisor.cpp
    src/network_aware_storage_factory.cpp
    src/network_share_access.cpp
    src/pe_restore_job_service.cpp
    src/pe_secret_sealer.cpp
    src/boot_check_supervisor.cpp
    src/post_backup_coordinator.cpp
    src/recovery_point_layout_service.cpp
    src/schedule_engine.cpp
    src/schedule_next_run.cpp
    src/schedule_service.cpp
    src/service_host.cpp
    src/service_log_formatter.cpp
    src/service_response_fit.cpp
    src/service_security_host.cpp
    src/service_session_executor.cpp
    src/windows_service_control.cpp
    src/windows_service_control_win32.cpp
    src/windows_service_scm_host.cpp
    src/worker_job_service.cpp
    src/worker_job_service_verify.cpp
    src/worker_job_service_file_restore.cpp
    src/worker_job_service_restore.cpp
    src/worker_job_service_restore_shared.cpp
    src/worker_job_service_shrink_analyze.cpp
    src/worker_supervisor.cpp
    src/supervisor_worker_protocol.cpp
)

add_library(Aegra::AppService ALIAS aegra_app_service)

target_include_directories(
    aegra_app_service
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_app_service PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_app_service
    PUBLIC
        Aegra::AppServiceProtocol
        Aegra::Application
        Aegra::Base
        Aegra::Contracts
        Aegra::Ports
        Aegra::AdapterWindowsIpc
    PRIVATE
        Aegra::AdapterPersonalArchive
        Aegra::AdapterCryptoSodium
        Aegra::PersonalRepository
        Aegra::AdapterWindowsFilesystem
        Aegra::AdapterWindowsSystem
        Aegra::AdapterWindowsDisk
        Aegra::AdapterWindowsPe
        Aegra::NtfsResize
        nlohmann_json::nlohmann_json
        Advapi32
        Mpr
        Netapi32
        Ws2_32
        Wtsapi32
)
aegra_configure_target(aegra_app_service)

add_executable(aegra_service src/service_main.cpp)
target_compile_definitions(
    aegra_service
    PRIVATE
        AEGRA_APPLICATION_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_link_libraries(
    aegra_service
    PRIVATE Aegra::AppService Aegra::AdapterStorageLocal Aegra::AdapterWindowsIpc
            Aegra::AdapterSqlite Aegra::AdapterWindowsDisk Aegra::AdapterWindowsFilesystem
            Aegra::AdapterWindowsPe Aegra::AdapterWindowsProcess Aegra::AdapterWindowsSystem
            Aegra::AdapterVirtualBox Aegra::AdapterHyperV
            Aegra::ThirdPartySpdlog Shell32
)
set_target_properties(aegra_service PROPERTIES OUTPUT_NAME "AegraService")
aegra_configure_target(aegra_service)
aegra_add_version_resource(aegra_service "Aegra Image Service")

# The WinPE image payload needs the MSVC runtime redistributables (WinPE does not
# ship them and vcpkg applocal only deploys package DLLs). Copy exactly the
# toolchain's own redist set beside the service so the payload collector can
# inject it. Release CRT only — debug CRT is not redistributable, so Debug builds
# cannot produce a bootable PE image.
if(MSVC)
    set(CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS_SKIP TRUE)
    include(InstallRequiredSystemLibraries)
    foreach(aegra_crt_dll IN LISTS CMAKE_INSTALL_SYSTEM_RUNTIME_LIBS)
        add_custom_command(
            TARGET aegra_service
            POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${aegra_crt_dll}"
                    "$<TARGET_FILE_DIR:aegra_service>"
            COMMENT "Copy MSVC runtime ${aegra_crt_dll} beside AegraService"
        )
    endforeach()
endif()
