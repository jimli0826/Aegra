find_package(unofficial-sodium CONFIG REQUIRED)

add_library(aegra_adapter_crypto_sodium STATIC
    src/content_hash.cpp
    src/metadata_crypto.cpp
    src/secure_string.cpp
    src/sidecar_crypto.cpp
)

add_library(Aegra::AdapterCryptoSodium ALIAS aegra_adapter_crypto_sodium)

target_include_directories(
    aegra_adapter_crypto_sodium
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(
    aegra_adapter_crypto_sodium
    PUBLIC Aegra::Base
    PRIVATE unofficial-sodium::sodium
)

aegra_configure_target(aegra_adapter_crypto_sodium)
