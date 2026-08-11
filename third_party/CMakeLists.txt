add_library(aegra_third_party_spdlog INTERFACE)
add_library(Aegra::ThirdPartySpdlog ALIAS aegra_third_party_spdlog)

target_include_directories(
    aegra_third_party_spdlog
    SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>"
)

# MSF — header-only Shell Extension templates (vendored from msf-main include/msf).
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/msf/msf.h")
    message(FATAL_ERROR
        "Vendored MSF headers missing at third_party/msf/msf.h. "
        "Restore the vendor drop (see third_party/msf/README.md).")
endif()

add_library(aegra_third_party_msf INTERFACE)
add_library(Aegra::ThirdPartyMsf ALIAS aegra_third_party_msf)

target_include_directories(
    aegra_third_party_msf
    SYSTEM INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/msf>"
)

