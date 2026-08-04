add_library(aegra_base STATIC
    src/error.cpp
    src/uuid.cpp
)

add_library(Aegra::Base ALIAS aegra_base)

target_include_directories(
    aegra_base
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

aegra_configure_target(aegra_base)
