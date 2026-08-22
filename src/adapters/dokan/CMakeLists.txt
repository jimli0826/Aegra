if(NOT WIN32)
    return()
endif()

add_library(aegra_adapter_dokan STATIC
    src/crc32c.cpp
    src/cow_backing_store.cpp
    src/disk_image.cpp
    src/vhdx_disk_image.cpp
    src/aux_file_store.cpp
    src/dokan_file_system.cpp
    src/file_set_fs.cpp
    src/virt_disk_attach.cpp
    src/partition_filter.cpp
    src/disk_mount.cpp
)

add_library(Aegra::AdapterDokan ALIAS aegra_adapter_dokan)

target_include_directories(
    aegra_adapter_dokan
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${CMAKE_SOURCE_DIR}/third_party/dokan/include"
)

target_compile_definitions(aegra_adapter_dokan PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)

target_link_libraries(
    aegra_adapter_dokan
    PUBLIC Aegra::Base Aegra::Ports Aegra::Format
    PRIVATE
        Virtdisk
        Shell32
        Rpcrt4
        Advapi32
        "${CMAKE_SOURCE_DIR}/third_party/dokan/lib/dokan2.lib"
)

aegra_configure_target(aegra_adapter_dokan)
