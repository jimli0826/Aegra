add_library(aegra_adapter_windows_process STATIC
    src/windows_process_launcher.cpp
)

add_library(Aegra::AdapterWindowsProcess ALIAS aegra_adapter_windows_process)

target_include_directories(aegra_adapter_windows_process
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(aegra_adapter_windows_process
    PUBLIC
        Aegra::Ports
        Aegra::Base
    PRIVATE
        Kernel32
)

target_compile_definitions(aegra_adapter_windows_process
    PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

aegra_configure_target(aegra_adapter_windows_process)
