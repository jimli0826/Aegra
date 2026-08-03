add_library(aegra_adapter_memory STATIC
    src/memory_backup_store.cpp
    src/memory_block_io.cpp
    src/memory_object_storage.cpp
)

add_library(Aegra::AdapterMemory ALIAS aegra_adapter_memory)

target_include_directories(
    aegra_adapter_memory
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(aegra_adapter_memory PUBLIC Aegra::Ports)
aegra_configure_target(aegra_adapter_memory)
