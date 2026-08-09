add_library(aegra_adapter_windows_filesystem STATIC
    src/windows_file_handle.cpp
    src/windows_file_names.cpp
    src/windows_file_security.cpp
    src/windows_file_snapshot_view.cpp
    src/windows_file_tree_sink.cpp
    src/windows_file_source_browser.cpp
    src/windows_file_special_folders.cpp
)

add_library(Aegra::AdapterWindowsFilesystem ALIAS aegra_adapter_windows_filesystem)

target_include_directories(
    aegra_adapter_windows_filesystem
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(aegra_adapter_windows_filesystem PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_adapter_windows_filesystem
    PUBLIC Aegra::Base Aegra::Contracts Aegra::Ports Aegra::Format
    PRIVATE Advapi32 Shell32 Ole32
)
aegra_configure_target(aegra_adapter_windows_filesystem)
