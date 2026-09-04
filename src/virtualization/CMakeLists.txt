add_library(aegra_virtualization INTERFACE)
add_library(Aegra::Virtualization ALIAS aegra_virtualization)

target_include_directories(
    aegra_virtualization
    INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(aegra_virtualization INTERFACE Aegra::Base Aegra::Contracts)
target_compile_features(aegra_virtualization INTERFACE cxx_std_20)
