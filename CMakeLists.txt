cmake_minimum_required(VERSION 4.3.1)

project(Aegra VERSION 0.1.0 LANGUAGES CXX)

option(AEGRA_WARNINGS_AS_ERRORS "Treat Aegra compiler warnings as errors" ON)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(AegraTarget)

enable_testing()

add_subdirectory(src/base)
add_subdirectory(src/contracts)
add_subdirectory(src/ports)
add_subdirectory(tests)

add_test(
    NAME architecture.source_limits
    COMMAND
        "${CMAKE_COMMAND}"
        "-DAEGRA_SOURCE_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/CheckSourceLimits.cmake"
)
