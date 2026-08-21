add_library(aegra_pipeline STATIC
    src/backup_pipeline.cpp
    src/bounded_chunk_queue.cpp
    src/chunk_buffer_pool.cpp
    src/fixed_size_chunker.cpp
    src/file_set_backup_pipeline.cpp
    src/file_set_change_planner.cpp
    src/file_set_restore_pipeline.cpp
    src/restore_pipeline.cpp
    src/protected_range_block_sink.cpp
    src/verify_pipeline.cpp
)

add_library(Aegra::Pipeline ALIAS aegra_pipeline)

target_include_directories(
    aegra_pipeline
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(
    aegra_pipeline
    PUBLIC Aegra::Base Aegra::Contracts Aegra::Ports Aegra::Format
)
aegra_configure_target(aegra_pipeline)
