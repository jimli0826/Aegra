find_package(zstd CONFIG REQUIRED)

add_library(aegra_adapter_compression_zstd STATIC
    src/zstd_codec.cpp
)

add_library(Aegra::AdapterCompressionZstd ALIAS aegra_adapter_compression_zstd)

target_include_directories(
    aegra_adapter_compression_zstd
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_adapter_compression_zstd
    PUBLIC Aegra::Base
    PRIVATE zstd::libzstd
)

aegra_configure_target(aegra_adapter_compression_zstd)
