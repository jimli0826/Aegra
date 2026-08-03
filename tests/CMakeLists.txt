function(aegra_add_test test_name source_file)
    add_executable(${test_name} ${source_file})
    aegra_configure_target(${test_name})
    add_test(NAME ${test_name} COMMAND ${test_name})
endfunction()

aegra_add_test(aegra_base_tests base/result_test.cpp)
target_link_libraries(aegra_base_tests PRIVATE Aegra::Base)

aegra_add_test(aegra_contracts_tests contracts/job_test.cpp)
target_link_libraries(aegra_contracts_tests PRIVATE Aegra::Contracts)

aegra_add_test(aegra_ports_compile_test ports/ports_compile_test.cpp)
target_link_libraries(aegra_ports_compile_test PRIVATE Aegra::Ports)

aegra_add_test(aegra_format_tests format/manifest_codec_test.cpp)
target_link_libraries(aegra_format_tests PRIVATE Aegra::Format)

aegra_add_test(aegra_personal_archive_codec_tests format/personal_archive_codec_test.cpp)
target_link_libraries(aegra_personal_archive_codec_tests PRIVATE Aegra::Format)

aegra_add_test(
    aegra_personal_repository_tests
    personal_repository/personal_repository_test.cpp
)
target_link_libraries(aegra_personal_repository_tests PRIVATE Aegra::PersonalRepository)

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
    aegra_add_test(aegra_windows_disk_tests adapters/windows_disk_test.cpp)
    target_link_libraries(aegra_windows_disk_tests PRIVATE Aegra::AdapterWindowsDisk)

    aegra_add_test(aegra_windows_system_tests adapters/windows_system_test.cpp)
    target_link_libraries(aegra_windows_system_tests PRIVATE Aegra::AdapterWindowsSystem)

    aegra_add_test(aegra_windows_ipc_tests adapters/windows_ipc_test.cpp)
    target_compile_definitions(aegra_windows_ipc_tests PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_link_libraries(aegra_windows_ipc_tests PRIVATE Aegra::AdapterWindowsIpc)

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
