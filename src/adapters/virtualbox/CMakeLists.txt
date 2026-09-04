if(NOT WIN32)
    return()
endif()

add_library(aegra_adapter_virtualbox STATIC
    src/virtualbox_boot_check_provider.cpp
    src/virtualbox_command_runner.cpp
    src/virtualbox_paths.cpp
    src/virtualbox_discovery.cpp
    src/virtualbox_scavenge.cpp
)
add_library(Aegra::AdapterVirtualBox ALIAS aegra_adapter_virtualbox)

target_include_directories(
    aegra_adapter_virtualbox
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
    aegra_adapter_virtualbox
    PUBLIC Aegra::Base Aegra::Ports Aegra::Virtualization
    PRIVATE Wintrust Crypt32
)
target_compile_definitions(aegra_adapter_virtualbox PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
aegra_configure_target(aegra_adapter_virtualbox)
