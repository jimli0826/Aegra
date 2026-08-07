find_package(nlohmann_json CONFIG REQUIRED)

add_executable(aegra_mount_host
    src/mount_host_main.cpp
    src/mount_host_session.cpp
)

target_compile_definitions(
    aegra_mount_host
    PRIVATE
        AEGRA_APPLICATION_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_include_directories(
    aegra_mount_host
    PRIVATE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

target_link_libraries(
    aegra_mount_host
    PRIVATE
        Aegra::Base
        Aegra::Ports
        Aegra::Format
        Aegra::AdapterPersonalArchive
        Aegra::AdapterDokan
        Aegra::AdapterWindowsIpc
        nlohmann_json::nlohmann_json
)

aegra_configure_target(aegra_mount_host)

# Dokan runtime DLL next to the executable for local runs.
add_custom_command(
    TARGET aegra_mount_host
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/dokan/bin/dokan2.dll"
            "$<TARGET_FILE_DIR:aegra_mount_host>/dokan2.dll"
    COMMENT "Copy dokan2.dll beside aegra_mount_host"
)

# Service resolves mount host beside aegra_service.exe (resolve_mount_host_path).
# mount_host is configured after service, so wire the copy from this target.
if(TARGET aegra_service)
    add_dependencies(aegra_service aegra_mount_host)
    add_custom_command(
        TARGET aegra_mount_host
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:aegra_mount_host>"
                "$<TARGET_FILE_DIR:aegra_service>/$<TARGET_FILE_NAME:aegra_mount_host>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/third_party/dokan/bin/dokan2.dll"
                "$<TARGET_FILE_DIR:aegra_service>/dokan2.dll"
        COMMENT "Copy aegra_mount_host and dokan2.dll beside aegra_service"
    )
endif()
