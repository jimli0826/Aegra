add_library(aegra_adapter_windows_ipc STATIC
    src/windows_named_pipe_channel.cpp
)

add_library(Aegra::AdapterWindowsIpc ALIAS aegra_adapter_windows_ipc)

target_include_directories(
    aegra_adapter_windows_ipc
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_windows_ipc PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(aegra_adapter_windows_ipc PUBLIC Aegra::Ports)
aegra_configure_target(aegra_adapter_windows_ipc)
