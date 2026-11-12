file(MAKE_DIRECTORY "${ECU_TEST_DIR}")
set(trace "${ECU_TEST_DIR}/fault trace.csv")

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --dashboard ascii --csv "${trace}"
    RESULT_VARIABLE logged_result OUTPUT_VARIABLE logged_dashboard ERROR_VARIABLE logged_error)
if(NOT logged_result EQUAL 0)
    message(FATAL_ERROR "CLI CSV run failed\n${logged_error}")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --dashboard ascii
    RESULT_VARIABLE plain_result OUTPUT_VARIABLE plain_dashboard ERROR_VARIABLE plain_error)
if(NOT plain_result EQUAL 0 OR NOT logged_dashboard STREQUAL plain_dashboard)
    message(FATAL_ERROR "Logging changed the dashboard output\n${plain_error}")
endif()

execute_process(COMMAND "${ECU_CSV_TESTS}" cli_file "${trace}"
    RESULT_VARIABLE read_result ERROR_VARIABLE read_error)
if(NOT read_result EQUAL 0)
    message(FATAL_ERROR "CLI file round trip failed\n${read_error}")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --csv "${ECU_TEST_DIR}"
    RESULT_VARIABLE open_result OUTPUT_QUIET ERROR_VARIABLE open_error)
if(open_result EQUAL 0 OR NOT open_error MATCHES "No se pudo abrir el archivo CSV")
    message(FATAL_ERROR "The CLI accepted a directory as a CSV file\n${open_error}")
endif()

set(existing "${ECU_TEST_DIR}/existing.csv")
file(WRITE "${existing}" "existing content\n")
execute_process(COMMAND "${ECU_EXECUTABLE}" --csv "${existing}" --dashboard invalid
    RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_QUIET)
file(READ "${existing}" contents)
if(invalid_result EQUAL 0 OR NOT contents STREQUAL "existing content\n")
    message(FATAL_ERROR "Invalid arguments must not truncate an existing output file")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" --csv --dashboard ascii
    RESULT_VARIABLE missing_result OUTPUT_QUIET ERROR_QUIET)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "The CLI accepted a flag in place of the CSV path")
endif()
