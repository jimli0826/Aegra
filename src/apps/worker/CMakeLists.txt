add_library(aegra_app_worker_personal STATIC
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
)

aegra_configure_target(aegra_app_worker_personal)
