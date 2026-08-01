add_library(aegra_ports INTERFACE)
add_library(Aegra::Ports ALIAS aegra_ports)

target_include_directories(
    aegra_ports
    INTERFACE
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(aegra_ports INTERFACE Aegra::Base Aegra::Contracts)
target_compile_features(aegra_ports INTERFACE cxx_std_20)
