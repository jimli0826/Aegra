add_library(aegra_adapter_storage_local STATIC
    src/local_object_storage.cpp
    src/local_staged_writer.cpp
    src/local_storage_paths.cpp
)

add_library(Aegra::AdapterStorageLocal ALIAS aegra_adapter_storage_local)

target_include_directories(
    aegra_adapter_storage_local
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_compile_definitions(aegra_adapter_storage_local PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(aegra_adapter_storage_local PUBLIC Aegra::Ports)
aegra_configure_target(aegra_adapter_storage_local)
