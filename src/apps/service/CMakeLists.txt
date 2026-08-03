find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_app_service STATIC
    src/service_host.cpp
    src/service_protocol.cpp
    src/service_protocol_request_json.cpp
    src/service_protocol_response_json.cpp
)

add_library(Aegra::AppService ALIAS aegra_app_service)

target_include_directories(
    aegra_app_service
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_app_service
    PUBLIC Aegra::Application Aegra::Base Aegra::Contracts Aegra::Ports
    PRIVATE nlohmann_json::nlohmann_json
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
)
aegra_configure_target(aegra_service)
