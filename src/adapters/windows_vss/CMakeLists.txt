add_library(aegra_adapter_windows_vss STATIC
    src/vss_snapshot_core.cpp
    src/windows_vss_backend.cpp
    src/windows_vss_session.cpp
)

add_library(Aegra::AdapterWindowsVss ALIAS aegra_adapter_windows_vss)

target_include_directories(
    aegra_adapter_windows_vss
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_windows_vss PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(aegra_adapter_windows_vss PUBLIC Aegra::Base PRIVATE VssApi Ole32 OleAut32)
aegra_configure_target(aegra_adapter_windows_vss)
