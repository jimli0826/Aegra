function(aegra_add_test test_name source_file)
    add_executable(${test_name} ${source_file})
    aegra_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

aegra_add_test(aegra_base_tests base/result_test.cpp)
target_link_libraries(aegra_base_tests PRIVATE Aegra::Base)

aegra_add_test(aegra_contracts_tests contracts/job_test.cpp)
target_link_libraries(aegra_contracts_tests PRIVATE Aegra::Contracts)

aegra_add_test(aegra_service_contract_tests contracts/service_test.cpp)
target_link_libraries(aegra_service_contract_tests PRIVATE Aegra::Contracts)

aegra_add_test(aegra_ports_compile_test ports/ports_compile_test.cpp)
target_link_libraries(aegra_ports_compile_test PRIVATE Aegra::Ports)


aegra_add_test(aegra_sqlite_control_plane_tests adapters/sqlite_control_plane_test.cpp)
target_link_libraries(aegra_sqlite_control_plane_tests PRIVATE Aegra::AdapterSqlite)

aegra_add_test(aegra_sqlite_restore_preflight_tests adapters/sqlite_restore_preflight_test.cpp)
target_link_libraries(aegra_sqlite_restore_preflight_tests PRIVATE Aegra::AdapterSqlite)

aegra_add_test(
    aegra_personal_repository_query_tests
    application/personal_repository_query_test.cpp
)
target_link_libraries(
    aegra_personal_repository_query_tests
    PRIVATE Aegra::Application Aegra::AdapterMemory
)

aegra_add_test(
    aegra_s4_inventory_repository_api_tests
    application/s4_inventory_repository_api_test.cpp
)
target_link_libraries(
    aegra_s4_inventory_repository_api_tests
    PRIVATE Aegra::Application Aegra::AdapterMemory Aegra::AdapterSqlite
)

aegra_add_test(
    aegra_s5_recovery_point_operations_tests
    application/s5_recovery_point_operations_test.cpp
)
target_compile_definitions(aegra_s5_recovery_point_operations_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(
    aegra_s5_recovery_point_operations_tests
    PRIVATE
        Aegra::Application
        Aegra::AppService
        Aegra::AdapterMemory
        Aegra::AdapterSqlite
        Aegra::AdapterWindowsProcess
)

aegra_add_test(
    aegra_s6_restore_preflight_tests
    application/s6_restore_preflight_test.cpp
)
target_link_libraries(
    aegra_s6_restore_preflight_tests
    PRIVATE Aegra::Application Aegra::AdapterSqlite
)

aegra_add_test(aegra_format_tests format/manifest_codec_test.cpp)
target_link_libraries(aegra_format_tests PRIVATE Aegra::Format)

aegra_add_test(aegra_personal_archive_codec_tests format/personal_archive_codec_test.cpp)
target_link_libraries(aegra_personal_archive_codec_tests PRIVATE Aegra::Format)

aegra_add_test(
    aegra_personal_repository_tests
    personal_repository/personal_repository_test.cpp
)
target_link_libraries(aegra_personal_repository_tests PRIVATE Aegra::PersonalRepository)
target_link_libraries(aegra_personal_repository_tests PRIVATE Aegra::AdapterMemory)

aegra_add_test(
    aegra_personal_repository_scanner_tests
    personal_repository/catalog_scanner_test.cpp
)
target_link_libraries(
    aegra_personal_repository_scanner_tests
    PRIVATE Aegra::PersonalRepository Aegra::AdapterMemory
)

aegra_add_test(aegra_memory_adapter_tests adapters/memory_adapter_test.cpp)
target_link_libraries(aegra_memory_adapter_tests PRIVATE Aegra::AdapterMemory)

aegra_add_test(
    aegra_memory_object_storage_tests
    adapters/memory_object_storage_test.cpp
)
target_include_directories(
    aegra_memory_object_storage_tests
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(aegra_memory_object_storage_tests PRIVATE Aegra::AdapterMemory)

aegra_add_test(aegra_crypto_sodium_tests adapters/crypto_sodium_test.cpp)
target_link_libraries(aegra_crypto_sodium_tests PRIVATE Aegra::AdapterCryptoSodium)

aegra_add_test(aegra_compression_zstd_tests adapters/compression_zstd_test.cpp)
target_link_libraries(aegra_compression_zstd_tests PRIVATE Aegra::AdapterCompressionZstd)

aegra_add_test(aegra_personal_archive_tests adapters/personal_archive_test.cpp)
target_link_libraries(
    aegra_personal_archive_tests
    PRIVATE Aegra::AdapterPersonalArchive Aegra::AdapterMemory Aegra::Pipeline
)

if(WIN32)
    aegra_add_test(aegra_storage_local_tests adapters/storage_local_test.cpp)
    target_include_directories(
        aegra_storage_local_tests
        PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
    )
    target_compile_definitions(aegra_storage_local_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aegra_storage_local_tests PRIVATE Aegra::AdapterStorageLocal)

    aegra_add_test(aegra_windows_disk_tests adapters/windows_disk_test.cpp)
    target_link_libraries(aegra_windows_disk_tests PRIVATE Aegra::AdapterWindowsDisk)

    aegra_add_test(aegra_windows_system_tests adapters/windows_system_test.cpp)
    target_link_libraries(aegra_windows_system_tests PRIVATE Aegra::AdapterWindowsSystem)

    aegra_add_test(aegra_windows_ipc_tests adapters/windows_ipc_test.cpp)
    target_compile_definitions(aegra_windows_ipc_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aegra_windows_ipc_tests PRIVATE Aegra::AdapterWindowsIpc)

    aegra_add_test(aegra_windows_ipc_security_tests adapters/windows_ipc_security_test.cpp)
    target_compile_definitions(aegra_windows_ipc_security_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aegra_windows_ipc_security_tests PRIVATE Aegra::AdapterWindowsIpc)

    aegra_add_test(aegra_service_protocol_tests apps/service_protocol_test.cpp)
    target_link_libraries(aegra_service_protocol_tests PRIVATE Aegra::AppService)

    aegra_add_test(aegra_service_security_host_tests apps/service_security_host_test.cpp)
    target_compile_definitions(aegra_service_security_host_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aegra_service_security_host_tests PRIVATE Aegra::AppService)


    add_executable(aegra_service_process_tests apps/service_process_test.cpp)
    aegra_configure_target(aegra_service_process_tests)
    target_compile_definitions(aegra_service_process_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(
        aegra_service_process_tests
        PRIVATE Aegra::AppService Aegra::AdapterWindowsIpc Aegra::PersonalRepository
    )
    add_dependencies(aegra_service_process_tests aegra_service)
    add_test(
        NAME aegra_service_process_tests
        COMMAND
            $<TARGET_FILE:aegra_service_process_tests>
            $<TARGET_FILE:aegra_service>
    )

    aegra_add_test(aegra_service_composition_tests apps/service_composition_test.cpp)
    target_compile_definitions(aegra_service_composition_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(
        aegra_service_composition_tests
        PRIVATE Aegra::AppService Aegra::AdapterSqlite
    )

    add_executable(aegra_worker_supervisor_process_tests apps/worker_supervisor_process_test.cpp)
    aegra_configure_target(aegra_worker_supervisor_process_tests)
    target_compile_definitions(
        aegra_worker_supervisor_process_tests
        PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN
    )
    target_link_libraries(
        aegra_worker_supervisor_process_tests
        PRIVATE Aegra::AppService Aegra::AdapterSqlite Aegra::AdapterWindowsProcess
    )
    add_dependencies(aegra_worker_supervisor_process_tests aegra_personal_worker)
    add_test(
        NAME aegra_worker_supervisor_process_tests
        COMMAND
            $<TARGET_FILE:aegra_worker_supervisor_process_tests>
            $<TARGET_FILE:aegra_personal_worker>
    )

    if(AEGRA_BUILD_DESKTOP)
        find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Network Qml)
        set(AEGRA_DESKTOP_CLIENT_SOURCES
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/ipc_frame_transport.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/ipc_frame_transport.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_client.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_client.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_client_backup.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_protocol.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_protocol.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_protocol_commands.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_protocol_detail.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_request_coordinator.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/service_request_coordinator.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/job_model.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/job_model.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/recovery_point_model.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/recovery_point_model.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/source_inventory_model.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/source_inventory_model.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/repository_connection_model.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/client/models/repository_connection_model.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_controller.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_controller.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_format.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_format.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/message_code_map.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/message_code_map.h"
        )
        add_executable(
            aegra_desktop_service_client_tests
            apps/desktop_service_client_test.cpp
            apps/desktop_client_test_support.h
            ${AEGRA_DESKTOP_CLIENT_SOURCES}
        )
        set_target_properties(aegra_desktop_service_client_tests PROPERTIES AUTOMOC ON)
        target_include_directories(
            aegra_desktop_service_client_tests
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src/apps/desktop/src"
                "${PROJECT_SOURCE_DIR}/tests/apps"
        )
        target_link_libraries(
            aegra_desktop_service_client_tests
            PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml
        )
        aegra_configure_target(aegra_desktop_service_client_tests)
        add_test(NAME aegra_desktop_service_client_tests COMMAND aegra_desktop_service_client_tests)
        set_tests_properties(
            aegra_desktop_service_client_tests
            PROPERTIES ENVIRONMENT_MODIFICATION
                       "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        add_executable(
            aegra_desktop_job_client_tests
            apps/desktop_job_client_test.cpp
            apps/desktop_client_test_support.h
            ${AEGRA_DESKTOP_CLIENT_SOURCES}
        )
        set_target_properties(aegra_desktop_job_client_tests PROPERTIES AUTOMOC ON)
        target_include_directories(
            aegra_desktop_job_client_tests
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src/apps/desktop/src"
                "${PROJECT_SOURCE_DIR}/tests/apps"
        )
        target_link_libraries(
            aegra_desktop_job_client_tests
            PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml
        )
        aegra_configure_target(aegra_desktop_job_client_tests)
        add_test(NAME aegra_desktop_job_client_tests COMMAND aegra_desktop_job_client_tests)
        set_tests_properties(
            aegra_desktop_job_client_tests
            PROPERTIES ENVIRONMENT_MODIFICATION
                       "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        add_executable(
            aegra_desktop_backup_client_tests
            apps/desktop_backup_client_test.cpp
            apps/desktop_client_test_support.h
            ${AEGRA_DESKTOP_CLIENT_SOURCES}
        )
        set_target_properties(aegra_desktop_backup_client_tests PROPERTIES AUTOMOC ON)
        target_include_directories(
            aegra_desktop_backup_client_tests
            PRIVATE
                "${PROJECT_SOURCE_DIR}/src/apps/desktop/src"
                "${PROJECT_SOURCE_DIR}/tests/apps"
        )
        target_link_libraries(
            aegra_desktop_backup_client_tests
            PRIVATE Qt6::Core Qt6::Gui Qt6::Network Qt6::Qml
        )
        aegra_configure_target(aegra_desktop_backup_client_tests)
        add_test(NAME aegra_desktop_backup_client_tests COMMAND aegra_desktop_backup_client_tests)
        set_tests_properties(
            aegra_desktop_backup_client_tests
            PROPERTIES ENVIRONMENT_MODIFICATION
                       "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        add_executable(
            aegra_desktop_locale_tests
            apps/desktop_locale_test.cpp
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_controller.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_controller.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_format.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/locale_format.h"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/message_code_map.cpp"
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/src/locale/message_code_map.h"
        )
        set_target_properties(aegra_desktop_locale_tests PROPERTIES AUTOMOC ON)
        target_include_directories(
            aegra_desktop_locale_tests
            PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/desktop/src"
        )
        target_compile_definitions(
            aegra_desktop_locale_tests
            PRIVATE
                "AEGRA_DESKTOP_I18N_DIR=\"${PROJECT_SOURCE_DIR}/src/apps/desktop/translations\""
        )
        target_link_libraries(
            aegra_desktop_locale_tests
            PRIVATE Qt6::Core Qt6::Gui Qt6::Qml
        )
        aegra_configure_target(aegra_desktop_locale_tests)
        add_test(NAME aegra_desktop_locale_tests COMMAND aegra_desktop_locale_tests)
        set_tests_properties(
            aegra_desktop_locale_tests
            PROPERTIES ENVIRONMENT_MODIFICATION
                       "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )

        # D2 multi-viewport / multi-locale visual smoke: loads Main.qml, resizes, switches
        # locales, and writes PNG evidence under docs/migration/evidence/d2/.
        find_package(Qt6 6.8 REQUIRED COMPONENTS Quick QuickControls2)
        set(AEGRA_D2_EVIDENCE_DIR
            "${PROJECT_SOURCE_DIR}/docs/migration/evidence/d2"
        )
        add_executable(
            aegra_desktop_visual_smoke_tests
            apps/desktop_visual_smoke_test.cpp
            ${AEGRA_DESKTOP_CLIENT_SOURCES}
            "${PROJECT_SOURCE_DIR}/src/apps/desktop/resources/resources.qrc"
        )
        set_target_properties(
            aegra_desktop_visual_smoke_tests
            PROPERTIES
                AUTOMOC ON
                AUTORCC ON
        )
        target_include_directories(
            aegra_desktop_visual_smoke_tests
            PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/desktop/src"
        )
        target_compile_definitions(
            aegra_desktop_visual_smoke_tests
            PRIVATE
                "AEGRA_D2_EVIDENCE_DIR=\"${AEGRA_D2_EVIDENCE_DIR}\""
        )
        target_link_libraries(
            aegra_desktop_visual_smoke_tests
            PRIVATE
                Qt6::Core
                Qt6::Gui
                Qt6::Network
                Qt6::Qml
                Qt6::Quick
                Qt6::QuickControls2
        )
        aegra_configure_target(aegra_desktop_visual_smoke_tests)
        add_test(
            NAME aegra_desktop_visual_smoke_tests
            COMMAND aegra_desktop_visual_smoke_tests
        )
        set_tests_properties(
            aegra_desktop_visual_smoke_tests
            PROPERTIES
                ENVIRONMENT_MODIFICATION
                    "PATH=path_list_prepend:$<TARGET_FILE_DIR:Qt6::Core>"
        )
    endif()

    aegra_add_test(aegra_windows_vss_tests adapters/windows_vss_test.cpp)
    target_include_directories(
        aegra_windows_vss_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/adapters/windows_vss/src"
    )
    target_link_libraries(aegra_windows_vss_tests PRIVATE Aegra::AdapterWindowsVss)

    aegra_add_test(aegra_windows_personal_backup_tests apps/windows_personal_backup_test.cpp)
    target_include_directories(
        aegra_windows_personal_backup_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_windows_personal_backup_tests
        PRIVATE Aegra::AppWorkerPersonal Aegra::Format
    )

    aegra_add_test(
        aegra_windows_personal_backup_task_tests
        apps/windows_personal_backup_task_test.cpp
    )
    target_include_directories(
        aegra_windows_personal_backup_task_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_windows_personal_backup_task_tests
        PRIVATE Aegra::AppWorkerPersonal Aegra::Contracts
    )

    aegra_add_test(
        aegra_personal_archive_verify_task_tests
        apps/personal_archive_verify_task_test.cpp
    )
    target_include_directories(
        aegra_personal_archive_verify_task_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_personal_archive_verify_task_tests
        PRIVATE Aegra::AppWorkerPersonal Aegra::Contracts
    )

    aegra_add_test(
        aegra_personal_archive_restore_task_tests
        apps/personal_archive_restore_task_test.cpp
    )
    target_include_directories(
        aegra_personal_archive_restore_task_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_personal_archive_restore_task_tests
        PRIVATE Aegra::AppWorkerPersonal Aegra::Contracts
    )

    aegra_add_test(aegra_worker_host_tests apps/worker_host_test.cpp)
    target_include_directories(
        aegra_worker_host_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_worker_host_tests
        PRIVATE Aegra::AppWorkerPersonal Aegra::Contracts
    )

    aegra_add_test(aegra_worker_protocol_tests apps/worker_protocol_test.cpp)
    target_link_libraries(
        aegra_worker_protocol_tests
        PRIVATE Aegra::AppWorkerPersonal
    )

    aegra_add_test(aegra_worker_session_tests apps/worker_session_test.cpp)
    target_include_directories(
        aegra_worker_session_tests
        PRIVATE "${PROJECT_SOURCE_DIR}/src/apps/worker/src"
    )
    target_link_libraries(
        aegra_worker_session_tests
        PRIVATE Aegra::AppWorkerPersonal
    )

    add_test(
        NAME aegra_personal_worker_rejection
        COMMAND
            "${CMAKE_COMMAND}"
            "-DAEGRA_WORKER=$<TARGET_FILE:aegra_personal_worker>"
            "-DAEGRA_REQUEST=${CMAKE_CURRENT_SOURCE_DIR}/apps/invalid_worker_job.json"
            -P "${PROJECT_SOURCE_DIR}/cmake/RunWorkerRejection.cmake"
    )

    add_executable(aegra_personal_worker_pipe_tests apps/personal_worker_pipe_test.cpp)
    aegra_configure_target(aegra_personal_worker_pipe_tests)
    target_compile_definitions(aegra_personal_worker_pipe_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(
        aegra_personal_worker_pipe_tests
        PRIVATE Aegra::Contracts
    )
    add_dependencies(aegra_personal_worker_pipe_tests aegra_personal_worker)
    add_test(
        NAME aegra_personal_worker_pipe_tests
        COMMAND
            $<TARGET_FILE:aegra_personal_worker_pipe_tests>
            $<TARGET_FILE:aegra_personal_worker>
    )
endif()

aegra_add_test(aegra_pipeline_tests pipeline/pipeline_roundtrip_test.cpp)
target_link_libraries(
    aegra_pipeline_tests
    PRIVATE Aegra::Pipeline Aegra::AdapterMemory
)

aegra_add_test(aegra_verify_pipeline_tests pipeline/verify_pipeline_test.cpp)
target_link_libraries(aegra_verify_pipeline_tests PRIVATE Aegra::Pipeline)
