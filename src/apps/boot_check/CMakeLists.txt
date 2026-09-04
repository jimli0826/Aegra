find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_app_boot_check STATIC
    src/boot_check_host.cpp
    src/boot_check_protocol.cpp
    src/boot_check_scavenge.cpp
    src/boot_check_task.cpp
)
add_library(Aegra::AppBootCheck ALIAS aegra_app_boot_check)

target_include_directories(
    aegra_app_boot_check
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

target_compile_definitions(aegra_app_boot_check PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_app_boot_check
    PUBLIC Aegra::Base Aegra::Contracts Aegra::Ports
    PRIVATE
        Aegra::AdapterDokan
        Aegra::AdapterHyperV
        Aegra::AdapterPersonalArchive
        Aegra::AdapterVirtualBox
        Aegra::AppWorkerTaskLog
        Aegra::Format
        Aegra::Virtualization
        nlohmann_json::nlohmann_json
)
aegra_configure_target(aegra_app_boot_check)

add_executable(aegra_boot_check_host src/boot_check_main.cpp)
target_compile_definitions(aegra_boot_check_host PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_boot_check_host
    PRIVATE
        Aegra::AppBootCheck
        Aegra::AdapterHyperV
        Aegra::AdapterVirtualBox
        Aegra::AdapterWindowsProcess
        Aegra::AdapterWindowsSystem
        Shell32
        delayimp
)
# The Dokan runtime only loads when a job actually presents a VMDK, so a host
# started for diagnostics on a machine without Dokan can still reject cleanly.
target_link_options(aegra_boot_check_host PRIVATE "/DELAYLOAD:dokan2.dll")
set_target_properties(aegra_boot_check_host PROPERTIES OUTPUT_NAME "AegraBootCheck")
aegra_configure_target(aegra_boot_check_host)
aegra_add_version_resource(aegra_boot_check_host "Aegra Image Boot Check")

# Dokan runtime DLL next to the executable for local runs.
add_custom_command(
    TARGET aegra_boot_check_host
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/third_party/dokan/bin/dokan2.dll"
            "$<TARGET_FILE_DIR:aegra_boot_check_host>/dokan2.dll"
    COMMENT "Copy dokan2.dll beside AegraBootCheck"
)

# Service resolves AegraBootCheck beside AegraService (resolve_sibling_executable).
if(TARGET aegra_service)
    add_custom_command(
        TARGET aegra_boot_check_host
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:aegra_boot_check_host>"
                "$<TARGET_FILE_DIR:aegra_service>/$<TARGET_FILE_NAME:aegra_boot_check_host>"
        COMMENT "Copy AegraBootCheck beside AegraService"
    )
endif()
