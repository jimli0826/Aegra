set(AEGRA_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" CACHE INTERNAL "")
set(AEGRA_VERSION_INCLUDE_DIR "${CMAKE_CURRENT_LIST_DIR}/../include" CACHE INTERNAL "")

function(aegra_configure_target target_name)
    target_compile_features(${target_name} PUBLIC cxx_std_20)

    if(MSVC)
        target_compile_options(
            ${target_name}
            PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8
        )
        if(AEGRA_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target_name} PRIVATE -Wall -Wextra -Wpedantic)
        if(AEGRA_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()
    endif()
endfunction()

function(aegra_add_version_resource target description)
    if(NOT WIN32)
        return()
    endif()

    get_target_property(_aegra_type ${target} TYPE)
    if(_aegra_type STREQUAL "SHARED_LIBRARY")
        set(AEGRI_FILETYPE "VFT_DLL")
        set(_aegra_ext "dll")
    elseif(_aegra_type STREQUAL "EXECUTABLE")
        set(AEGRI_FILETYPE "VFT_APP")
        set(_aegra_ext "exe")
    else()
        message(FATAL_ERROR "aegra_add_version_resource applies only to EXE or shared DLL: ${target}")
    endif()

    get_target_property(_aegra_out ${target} OUTPUT_NAME)
    if(NOT _aegra_out)
        set(_aegra_out "${target}")
    endif()

    set(AEGRI_FILE_DESCRIPTION "${description}")
    set(AEGRI_INTERNAL_NAME "${_aegra_out}")
    set(AEGRI_ORIGINAL_FILENAME "${_aegra_out}.${_aegra_ext}")
    set(_aegra_rc "${CMAKE_CURRENT_BINARY_DIR}/${target}_version.rc")
    configure_file(
        "${AEGRA_CMAKE_DIR}/aegra_target_version.rc.in"
        "${_aegra_rc}"
        @ONLY
    )
    set_source_files_properties(
        "${_aegra_rc}"
        PROPERTIES INCLUDE_DIRECTORIES "${AEGRA_VERSION_INCLUDE_DIR}"
    )
    target_sources(${target} PRIVATE "${_aegra_rc}")
    target_include_directories(${target} PRIVATE "${AEGRA_VERSION_INCLUDE_DIR}")
endfunction()
