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
