find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_app_worker_personal STATIC
    src/personal_archive_restore_task.cpp
    src/personal_archive_restore_task_backend.cpp
    src/personal_archive_restore_shrink.cpp
    src/personal_archive_verify_task.cpp
    src/personal_archive_verify_task_backend.cpp
    src/personal_file_archive_restore_task.cpp
    src/personal_file_archive_verify_task.cpp
    src/worker_host.cpp
    src/worker_protocol.cpp
    src/worker_session.cpp
    src/worker_session_protocol.cpp
    src/worker_task_log.cpp
    src/windows_file_set_backup.cpp
    src/windows_file_set_backup_task.cpp
    src/windows_personal_backup.cpp
    src/windows_personal_backup_runtime.cpp
    src/windows_personal_backup_task.cpp
    src/windows_personal_backup_task_backend.cpp
)

add_library(Aegra::AppWorkerPersonal ALIAS aegra_app_worker_personal)

target_include_directories(
    aegra_app_worker_personal
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_app_worker_personal PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_app_worker_personal
    PUBLIC Aegra::Base Aegra::Pipeline Aegra::Ports
    PRIVATE
        Aegra::AdapterCryptoSodium
        Aegra::AdapterPersonalArchive
        Aegra::AdapterStorageLocal
        Aegra::AdapterWindowsDisk
        Aegra::AdapterWindowsFilesystem
        Aegra::AdapterWindowsProcess
        Aegra::AdapterWindowsVss
        Aegra::Format
        Aegra::NtfsResize
        Aegra::ThirdPartySpdlog
        nlohmann_json::nlohmann_json
)

aegra_configure_target(aegra_app_worker_personal)

add_executable(aegra_personal_worker
    src/personal_worker_main.cpp
    src/mount_host_session.cpp
)
target_compile_definitions(
    aegra_personal_worker
    PRIVATE
        AEGRA_APPLICATION_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)
target_link_libraries(
    aegra_personal_worker
    PRIVATE
        Aegra::AppWorkerPersonal
        Aegra::AdapterDokan
        Aegra::AdapterPersonalArchive
        Aegra::AdapterWindowsIpc
        Aegra::AdapterWindowsSystem
        nlohmann_json::nlohmann_json
        delayimp
)
# Dokan is only touched in --mount-pipe mode; delay-load keeps backup/restore
# tasks runnable on machines without dokan2.dll.
target_link_options(aegra_personal_worker PRIVATE "/DELAYLOAD:dokan2.dll")
aegra_configure_target(aegra_personal_worker)

# Dokan runtime DLL next to the executable for local runs.
add_custom_command(
    TARGET aegra_personal_worker
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/dokan/bin/dokan2.dll"
            "$<TARGET_FILE_DIR:aegra_personal_worker>/dokan2.dll"
    COMMENT "Copy dokan2.dll beside aegra_personal_worker"
)

# Service resolves the worker beside aegra_service.exe (resolve_sibling_executable).
# worker is configured after service, so wire the copy from this target.
if(TARGET aegra_service)
    add_dependencies(aegra_service aegra_personal_worker)
    add_custom_command(
        TARGET aegra_personal_worker
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:aegra_personal_worker>"
                "$<TARGET_FILE_DIR:aegra_service>/$<TARGET_FILE_NAME:aegra_personal_worker>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/dokan/bin/dokan2.dll"
                "$<TARGET_FILE_DIR:aegra_service>/dokan2.dll"
        COMMENT "Copy aegra_personal_worker and dokan2.dll beside aegra_service"
    )
endif()
