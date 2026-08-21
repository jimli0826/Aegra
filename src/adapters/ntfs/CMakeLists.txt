add_library(aegra_adapter_ntfs STATIC
    src/ntfs_adapter_convert.cpp
    src/ntfs_file_layout.cpp
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
    PUBLIC Aegra::Base Aegra::Ports Aegra::NtfsCore
)

aegra_configure_target(aegra_adapter_ntfs)
