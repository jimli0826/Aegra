if(NOT DEFINED AEGRA_WORKER OR NOT DEFINED AEGRA_REQUEST)
    message(FATAL_ERROR "AEGRA_WORKER and AEGRA_REQUEST are required")
endif()

execute_process(
    COMMAND "${AEGRA_WORKER}"
    INPUT_FILE "${AEGRA_REQUEST}"
    OUTPUT_VARIABLE response
    ERROR_VARIABLE error_output
    RESULT_VARIABLE exit_code
    TIMEOUT 10
)

if(NOT exit_code EQUAL 20)
    message(FATAL_ERROR "worker returned ${exit_code}; stderr=${error_output}")
endif()

if(NOT response MATCHES "\"kind\":2" OR
   NOT response MATCHES "\"message_code\":\"worker.request_rejected\"")
    message(FATAL_ERROR "worker did not emit a structured rejection: ${response}")
endif()
