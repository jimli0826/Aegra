cmake_minimum_required(VERSION 3.20)

if(NOT COMMAND aegra_configure_target)
    project(aegra_desktop LANGUAGES CXX)
    list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/../../../cmake")
    include(AegraTarget)
endif()

find_package(
    Qt6 6.8 REQUIRED
    COMPONENTS Core Gui Network Qml Quick QuickControls2 LinguistTools
)

set(AEGRA_DESKTOP_SOURCES
    resources/desktop.rc
    resources/resources.qrc
    src/main.cpp
    src/client/ipc_frame_transport.cpp
    src/client/ipc_frame_transport.h
    src/client/service_client.cpp
    src/client/service_client.h
    src/client/service_protocol.cpp
    src/client/service_protocol.h
    src/client/service_protocol_commands.cpp
    src/client/service_protocol_detail.h
    src/client/service_request_coordinator.cpp
    src/client/service_request_coordinator.h
    src/client/service_client_backup.cpp
    src/client/service_client_file.cpp
    src/client/service_client_mount.cpp
    src/client/service_client_repository.cpp
    src/client/service_client_restore.cpp
    src/client/service_client_schedule.cpp
    src/client/service_protocol_file.cpp
    src/client/models/job_model.cpp
    src/client/models/job_model.h
    src/client/models/file_browse_model.cpp
    src/client/models/file_browse_model.h
    src/client/models/file_recover_model.cpp
    src/client/models/file_recover_model.h
    src/client/models/recovery_point_model.cpp
    src/client/models/recovery_point_model.h
    src/client/models/source_inventory_model.cpp
    src/client/models/source_inventory_model.h
    src/client/models/repository_connection_model.cpp
    src/client/models/repository_connection_model.h
    src/client/models/schedule_list_model.cpp
    src/client/models/schedule_list_model.h
    src/locale/locale_controller.cpp
    src/locale/locale_controller.h
    src/locale/locale_format.cpp
    src/locale/locale_format.h
    src/locale/message_code_map.cpp
    src/locale/message_code_map.h
)

set(AEGRA_DESKTOP_TS_FILES
    translations/aegra_desktop_en_US.ts
    translations/aegra_desktop_zh_CN.ts
    translations/aegra_desktop_zh_TW.ts
    translations/aegra_desktop_ja_JP.ts
    translations/aegra_desktop_de_DE.ts
)

set(AEGRA_DESKTOP_QM_FILES)
foreach(ts_file IN LISTS AEGRA_DESKTOP_TS_FILES)
    get_filename_component(ts_name "${ts_file}" NAME_WE)
    set(qm_file "${CMAKE_CURRENT_SOURCE_DIR}/translations/${ts_name}.qm")
    add_custom_command(
        OUTPUT "${qm_file}"
        COMMAND Qt6::lrelease
                -idbased
                "${CMAKE_CURRENT_SOURCE_DIR}/${ts_file}"
                -qm "${qm_file}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${ts_file}"
        COMMENT "lrelease -idbased ${ts_name}"
        VERBATIM
    )
    list(APPEND AEGRA_DESKTOP_QM_FILES "${qm_file}")
endforeach()

add_custom_target(aegra_desktop_translations DEPENDS ${AEGRA_DESKTOP_QM_FILES})

add_executable(aegra_desktop WIN32 ${AEGRA_DESKTOP_SOURCES})
add_dependencies(aegra_desktop aegra_desktop_translations)

set_target_properties(
    aegra_desktop
    PROPERTIES
        AUTOMOC ON
        AUTORCC ON
)

target_include_directories(
    aegra_desktop
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_link_libraries(
    aegra_desktop
    PRIVATE
        Qt6::Core
        Qt6::Gui
        Qt6::Network
        Qt6::Qml
        Qt6::Quick
        Qt6::QuickControls2
)

if(WIN32)
    target_link_libraries(aegra_desktop PRIVATE dwmapi)
endif()

aegra_configure_target(aegra_desktop)
