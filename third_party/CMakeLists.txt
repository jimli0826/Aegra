add_library(aegra_third_party_spdlog INTERFACE)
add_library(Aegra::ThirdPartySpdlog ALIAS aegra_third_party_spdlog)

target_include_directories(
    aegra_third_party_spdlog
    SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

