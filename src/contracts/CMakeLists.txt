add_library(aegra_contracts STATIC
    src/job.cpp
    src/progress.cpp
    src/service.cpp
    src/task_result.cpp
    src/worker_response.cpp
    src/worker_session.cpp
)

add_library(Aegra::Contracts ALIAS aegra_contracts)

target_include_directories(
    aegra_contracts
    PUBLIC
        "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
)

target_link_libraries(aegra_contracts PUBLIC Aegra::Base)
aegra_configure_target(aegra_contracts)
