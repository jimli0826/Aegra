find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_adapter_windows_pe STATIC
    src/pe_archive_location.cpp
    src/pe_boot_commands.cpp
    src/pe_boot_controller.cpp
    src/pe_image_builder.cpp
    src/pe_image_sources.cpp
    src/pe_pending_files.cpp
    src/pe_pending_json.cpp
    src/pe_pending_store.cpp
    src/pe_session_log.cpp
    src/pe_volume_scan.cpp
)

add_library(Aegra::AdapterWindowsPe ALIAS aegra_adapter_windows_pe)

target_include_directories(
    aegra_adapter_windows_pe
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_windows_pe PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_adapter_windows_pe
    PUBLIC Aegra::Ports
    PRIVATE nlohmann_json::nlohmann_json Advapi32 Bcrypt
)
aegra_configure_target(aegra_adapter_windows_pe)
