file(MAKE_DIRECTORY "${ECU_TEST_DIR}")
set(trace "${ECU_TEST_DIR}/fault trace.bin")
file(WRITE "${trace}" "old contents\n")

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --dashboard ascii --binary "${trace}"
    RESULT_VARIABLE logged_result OUTPUT_VARIABLE logged_dashboard ERROR_VARIABLE logged_error)
if(NOT logged_result EQUAL 0)
    message(FATAL_ERROR "CLI binary run failed\n${logged_error}")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --dashboard ascii
    RESULT_VARIABLE plain_result OUTPUT_VARIABLE plain_dashboard ERROR_VARIABLE plain_error)
if(NOT plain_result EQUAL 0 OR NOT logged_dashboard STREQUAL plain_dashboard)
    message(FATAL_ERROR "Logging changed the dashboard output\n${plain_error}")
endif()

execute_process(COMMAND "${ECU_BINARY_TESTS}" cli_file "${trace}"
    RESULT_VARIABLE read_result ERROR_VARIABLE read_error)
if(NOT read_result EQUAL 0)
    message(FATAL_ERROR "CLI binary round trip failed\n${read_error}")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" faults --binary "${ECU_TEST_DIR}"
    RESULT_VARIABLE open_result OUTPUT_QUIET ERROR_VARIABLE open_error)
if(open_result EQUAL 0 OR NOT open_error MATCHES "No se pudo abrir el archivo binario")
    message(FATAL_ERROR "The CLI accepted a directory as a binary file\n${open_error}")
endif()

set(existing_binary "${ECU_TEST_DIR}/existing.bin")
set(existing_csv "${ECU_TEST_DIR}/existing.csv")
file(WRITE "${existing_binary}" "existing binary\n")
file(WRITE "${existing_csv}" "existing CSV\n")
execute_process(COMMAND "${ECU_EXECUTABLE}" --binary "${existing_binary}" --csv "${existing_csv}"
    RESULT_VARIABLE invalid_result OUTPUT_QUIET ERROR_QUIET)
file(READ "${existing_binary}" binary_contents)
file(READ "${existing_csv}" csv_contents)
if(invalid_result EQUAL 0 OR NOT binary_contents STREQUAL "existing binary\n" OR
   NOT csv_contents STREQUAL "existing CSV\n")
    message(FATAL_ERROR "Conflicting output options must be rejected before opening either file")
endif()

execute_process(COMMAND "${ECU_EXECUTABLE}" --binary --dashboard ascii
    RESULT_VARIABLE missing_result OUTPUT_QUIET ERROR_QUIET)
if(missing_result EQUAL 0)
    message(FATAL_ERROR "The CLI accepted a flag in place of the binary path")
endif()
