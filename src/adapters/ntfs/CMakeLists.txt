add_library(aegra_adapter_ntfs STATIC
    src/ntfs_validation.cpp
    src/ntfs_fixup.cpp
    src/ntfs_boot_sector.cpp
    src/ntfs_runlist.cpp
    src/ntfs_attribute_parser.cpp
    src/ntfs_record_parser.cpp
    src/ntfs_directory_index.cpp
    src/ntfs_volume_reader.cpp
)

add_library(Aegra::AdapterNtfs ALIAS aegra_adapter_ntfs)

target_include_directories(
    aegra_adapter_ntfs
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
)

target_link_libraries(
    aegra_adapter_ntfs
    PUBLIC Aegra::Base Aegra::Ports
)

aegra_configure_target(aegra_adapter_ntfs)
