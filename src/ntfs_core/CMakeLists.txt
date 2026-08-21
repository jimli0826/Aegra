add_library(aegra_ntfs_core STATIC
    src/binary.cpp
    src/fixup.cpp
    src/boot_sector.cpp
    src/runlist.cpp
    src/attribute.cpp
    src/mft_record.cpp
    src/attribute_list.cpp
    src/bitmap.cpp
    src/layout_read.cpp
)

add_library(Aegra::NtfsCore ALIAS aegra_ntfs_core)

target_include_directories(
    aegra_ntfs_core
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_ntfs_core
    PUBLIC Aegra::Base Aegra::Ports
)

aegra_configure_target(aegra_ntfs_core)
