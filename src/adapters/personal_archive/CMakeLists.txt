add_library(aegra_adapter_personal_archive STATIC
    src/personal_archive_block_worker_pool.cpp
    src/personal_archive_chunk_builder.cpp
    src/personal_archive_chain_reader.cpp
    src/personal_archive_reader.cpp
    src/personal_archive_volume_reader.cpp
    src/personal_archive_preamble.cpp
    src/personal_archive_payload.cpp
    src/personal_archive_session.cpp
    src/personal_file_archive_session.cpp
    src/personal_file_archive_secondary_index.cpp
    src/personal_file_archive_lazy_index.cpp
    src/personal_file_archive_reader.cpp
    src/personal_file_archive_chain_reader.cpp
    src/personal_archive_shape_validation.cpp
    src/personal_archive_sidecar_io.cpp
    src/whole_disk_byte_reader.cpp
)

add_library(Aegra::AdapterPersonalArchive ALIAS aegra_adapter_personal_archive)

target_include_directories(
    aegra_adapter_personal_archive
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_adapter_personal_archive
    PUBLIC Aegra::Base Aegra::Format Aegra::Ports
    PRIVATE Aegra::AdapterCompressionZstd Aegra::AdapterCryptoSodium
)

aegra_configure_target(aegra_adapter_personal_archive)
