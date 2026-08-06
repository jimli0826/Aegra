add_library(aegra_adapter_windows_system STATIC
    src/windows_clock.cpp
    src/windows_credential_resolver.cpp
    src/windows_random_source.cpp
)

add_library(Aegra::AdapterWindowsSystem ALIAS aegra_adapter_windows_system)

target_include_directories(
    aegra_adapter_windows_system
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_windows_system PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_adapter_windows_system
    PUBLIC Aegra::Ports
    PRIVATE Crypt32 Bcrypt
)
aegra_configure_target(aegra_adapter_windows_system)
