find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_app_worker_personal STATIC
    src/personal_archive_restore_task.cpp
    src/personal_archive_restore_task_backend.cpp
    src/personal_archive_verify_task.cpp
    src/personal_archive_verify_task_backend.cpp
    src/worker_host.cpp
    src/worker_protocol.cpp
    src/worker_session.cpp
    src/worker_session_protocol.cpp
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

target_link_libraries(
    aegra_app_worker_personal
    PUBLIC Aegra::Base Aegra::Pipeline Aegra::Ports
    PRIVATE
        Aegra::AdapterPersonalArchive
        Aegra::AdapterWindowsDisk
        Aegra::AdapterWindowsVss
        Aegra::Format
        nlohmann_json::nlohmann_json
)

aegra_configure_target(aegra_app_worker_personal)

add_executable(aegra_personal_worker src/personal_worker_main.cpp)
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
        Aegra::AdapterWindowsIpc
        Aegra::AdapterWindowsSystem
)
aegra_configure_target(aegra_personal_worker)
