add_library(aegra_ntfs_resize STATIC
    src/sha256.cpp
    src/shrink_plan_binary.cpp
    src/shrink_plan_payload.cpp
    src/shrink_plan_builder.cpp
    src/shrink_plan_codec.cpp
    src/ntfs_volume_view.cpp
    src/ntfs_mft_scanner.cpp
    src/ntfs_bitmap_allocator.cpp
    src/ntfs_relocation_plan.cpp
    src/ntfs_shrink_audit.cpp
    src/ntfs_shrink_analyzer.cpp
    src/sparse_overlay_index.cpp
    src/block_device_byte_io.cpp
    src/composite_ntfs_block_device.cpp
    src/boot_sector_invalidation.cpp
    src/ntfs_cluster_mover.cpp
    src/ntfs_record_writer.cpp
    src/ntfs_metadata_editor.cpp
    src/ntfs_bitmap_commit.cpp
    src/ntfs_ordinary_relocation.cpp
    src/ntfs_logfile_invalidation.cpp
    src/ntfs_mft_mirror_sync.cpp
    src/ntfs_critical_relocation.cpp
    src/ntfs_precommit_auditor.cpp
    src/ntfs_boot_commit.cpp
    src/ntfs_chkdsk_runner.cpp
    src/ntfs_shrink_finalize.cpp
)

add_library(Aegra::NtfsResize ALIAS aegra_ntfs_resize)

target_include_directories(
    aegra_ntfs_resize
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(
    aegra_ntfs_resize
    PUBLIC Aegra::Base Aegra::Ports Aegra::NtfsCore
)

aegra_configure_target(aegra_ntfs_resize)
