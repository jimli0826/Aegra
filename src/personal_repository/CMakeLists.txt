find_package(nlohmann_json CONFIG REQUIRED)

add_library(aegra_personal_repository STATIC
    src/catalog_codec.cpp
    src/catalog_validation.cpp
    src/chain_graph.cpp
    src/deletion_codec.cpp
    src/json_codec.cpp
)

add_library(Aegra::PersonalRepository ALIAS aegra_personal_repository)

target_include_directories(
    aegra_personal_repository
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_personal_repository
    PUBLIC Aegra::Base Aegra::Format
    PRIVATE nlohmann_json::nlohmann_json
)

aegra_configure_target(aegra_personal_repository)

