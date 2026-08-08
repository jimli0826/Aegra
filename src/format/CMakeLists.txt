find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_format STATIC
    src/manifest_codec.cpp
    src/manifest_validation.cpp
    src/personal_archive_codec.cpp
    src/personal_archive_sidecar.cpp
    src/file_index_codec.cpp
)

add_library(Aegra::Format ALIAS aegra_format)

target_include_directories(
    aegra_format
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_format
    PUBLIC Aegra::Base Aegra::Contracts
    PRIVATE nlohmann_json::nlohmann_json
)

aegra_configure_target(aegra_format)
