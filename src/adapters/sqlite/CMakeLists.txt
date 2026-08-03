add_library(aegra_adapter_sqlite STATIC
    src/sqlite_audit_event_store.cpp
    src/sqlite_control_plane.cpp
    src/sqlite_job_store.cpp
    src/sqlite_page_token.cpp
    src/sqlite_repository_connection_store.cpp
    src/sqlite_schedule_store.cpp
    src/sqlite_schema.cpp
    src/sqlite_support.cpp
)

add_library(Aegra::AdapterSqlite ALIAS aegra_adapter_sqlite)

target_include_directories(
    aegra_adapter_sqlite
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

find_package(unofficial-sqlite3 CONFIG REQUIRED)
target_link_libraries(
    aegra_adapter_sqlite
    PUBLIC Aegra::Ports
    PRIVATE unofficial::sqlite3::sqlite3
)
aegra_configure_target(aegra_adapter_sqlite)
