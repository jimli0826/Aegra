add_executable(aegra_cli
    src/main.cpp
    src/cli_args.cpp
    src/cli_commands.cpp
    src/cli_commands_list.cpp
    src/cli_commands_mutate.cpp
    src/cli_format.cpp
    src/cli_ids.cpp
    src/cli_io.cpp
    src/cli_session.cpp
)

target_include_directories(
    aegra_cli
    PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_definitions(
    aegra_cli
    PRIVATE
        AEGRA_APPLICATION_VERSION="${PROJECT_VERSION}"
        NOMINMAX
        WIN32_LEAN_AND_MEAN
)

target_link_libraries(
    aegra_cli
    PRIVATE
        Aegra::AppServiceProtocol
        Aegra::AdapterWindowsIpc
        Aegra::AdapterWindowsSystem
        Aegra::Base
        Aegra::Contracts
)

aegra_configure_target(aegra_cli)
aegra_add_version_resource(aegra_cli "Aegra Image CLI")

# Keep the CLI beside the Service binary for local runs.
if(TARGET aegra_service)
    add_custom_command(
        TARGET aegra_cli
        POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:aegra_cli>"
                "$<TARGET_FILE_DIR:aegra_service>/$<TARGET_FILE_NAME:aegra_cli>"
        COMMENT "Copy aegra_cli beside aegra_service"
    )
endif()
