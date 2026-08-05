find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_app_service STATIC
    src/backup_catalog_registrar.cpp
    src/schedule_service.cpp
    src/service_host.cpp
    src/service_log_formatter.cpp
    src/service_protocol.cpp
    src/service_protocol_request_json.cpp
    src/service_protocol_response_json.cpp
    src/service_security_host.cpp
    src/windows_service_control.cpp
    src/windows_service_control_win32.cpp
    src/windows_service_scm_host.cpp
    src/worker_job_service.cpp
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
        Aegra::Application
        Aegra::Base
        Aegra::Contracts
        Aegra::Ports
        Aegra::AdapterWindowsIpc
    PRIVATE nlohmann_json::nlohmann_json Advapi32
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
            Aegra::AdapterSqlite Aegra::AdapterWindowsDisk Aegra::AdapterWindowsProcess
            Aegra::AdapterWindowsSystem Aegra::ThirdPartySpdlog Shell32
)
aegra_configure_target(aegra_service)
