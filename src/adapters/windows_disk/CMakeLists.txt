add_library(aegra_adapter_windows_disk STATIC
    src/windows_block_source.cpp
    src/windows_block_sink.cpp
    src/windows_volume_enumerator.cpp
)

add_library(Aegra::AdapterWindowsDisk ALIAS aegra_adapter_windows_disk)

target_include_directories(
    aegra_adapter_windows_disk
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_windows_disk PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(aegra_adapter_windows_disk PUBLIC Aegra::Ports)
aegra_configure_target(aegra_adapter_windows_disk)
