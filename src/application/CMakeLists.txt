add_library(aegra_application STATIC
    src/personal_repository_query.cpp
)

add_library(Aegra::Application ALIAS aegra_application)

target_include_directories(
    aegra_application
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_application
    PUBLIC Aegra::Base Aegra::Contracts Aegra::PersonalRepository Aegra::Ports
)
aegra_configure_target(aegra_application)
