if(NOT WIN32)
    return()
endif()

add_library(aegra_adapter_hyperv STATIC
    src/hyperv_boot_check_provider.cpp
    src/hyperv_discovery.cpp
    src/hyperv_powershell.cpp
    src/hyperv_scavenge.cpp
)
add_library(Aegra::AdapterHyperV ALIAS aegra_adapter_hyperv)

target_include_directories(
    aegra_adapter_hyperv
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)
target_link_libraries(
    aegra_adapter_hyperv
    PUBLIC Aegra::Base Aegra::Ports Aegra::Virtualization
    PRIVATE Advapi32
)
target_compile_definitions(aegra_adapter_hyperv PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
aegra_configure_target(aegra_adapter_hyperv)
