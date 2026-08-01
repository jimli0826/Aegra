if(NOT DEFINED AEGRA_SOURCE_ROOT)
    message(FATAL_ERROR "AEGRA_SOURCE_ROOT is required")
endif()

file(
    GLOB_RECURSE AEGRA_SOURCES
    LIST_DIRECTORIES false
    "${AEGRA_SOURCE_ROOT}/src/*.cpp"
    "${AEGRA_SOURCE_ROOT}/src/*.h"
    "${AEGRA_SOURCE_ROOT}/src/*.hpp"
    "${AEGRA_SOURCE_ROOT}/tests/*.cpp"
    "${AEGRA_SOURCE_ROOT}/tests/*.h"
    "${AEGRA_SOURCE_ROOT}/tests/*.hpp"
)

set(AEGRA_VIOLATIONS "")

foreach(AEGRA_FILE IN LISTS AEGRA_SOURCES)
    file(READ "${AEGRA_FILE}" AEGRA_CONTENT)
    string(REGEX MATCHALL "\n" AEGRA_NEWLINES "${AEGRA_CONTENT}")
    list(LENGTH AEGRA_NEWLINES AEGRA_LINE_COUNT)
    math(EXPR AEGRA_LINE_COUNT "${AEGRA_LINE_COUNT} + 1")
    get_filename_component(AEGRA_EXTENSION "${AEGRA_FILE}" EXT)

    if(AEGRA_EXTENSION STREQUAL ".cpp")
        set(AEGRA_LIMIT 800)
    else()
        set(AEGRA_LIMIT 500)
    endif()

    if(AEGRA_LINE_COUNT GREATER AEGRA_LIMIT)
        list(APPEND AEGRA_VIOLATIONS "${AEGRA_FILE}: ${AEGRA_LINE_COUNT} > ${AEGRA_LIMIT}")
    endif()
endforeach()

if(AEGRA_VIOLATIONS)
    list(JOIN AEGRA_VIOLATIONS "\n" AEGRA_MESSAGE)
    message(FATAL_ERROR "Source size limits exceeded:\n${AEGRA_MESSAGE}")
endif()

message(STATUS "Aegra source file size limits passed")
