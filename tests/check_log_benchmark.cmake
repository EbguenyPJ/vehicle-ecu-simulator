file(MAKE_DIRECTORY "${ECU_TEST_DIR}")
string(RANDOM LENGTH 12 suffix)
set(trace_dir "${ECU_TEST_DIR}/${suffix}")
execute_process(COMMAND "${ECU_LOG_BENCHMARK}" "${trace_dir}" --verify
    RESULT_VARIABLE status OUTPUT_VARIABLE samples ERROR_VARIABLE error)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Log benchmark verification failed\n${error}")
endif()

# Los codecs ya verificaron los registros en memoria. Comprueba el informe y ambos archivos.
file(SIZE "${trace_dir}/trace.csv" csv_bytes)
file(SIZE "${trace_dir}/trace.bin" binary_bytes)
if(csv_bytes LESS 1 OR binary_bytes LESS 1)
    message(FATAL_ERROR "Expected both log files to be nonempty")
endif()

string(REGEX REPLACE "\r\n" "\n" samples "${samples}")
string(REGEX MATCH
    "^format,sample,scenario_copies,records,bytes,write_ns,read_ns\ncsv,1,2,84,${csv_bytes},[0-9]+,[0-9]+\nbinary,1,2,84,${binary_bytes},[0-9]+,[0-9]+\n$"
    valid_samples "${samples}")
if(NOT valid_samples)
    message(FATAL_ERROR "Unexpected log benchmark report\n${samples}")
endif()

execute_process(COMMAND "${ECU_LOG_BENCHMARK}" "${trace_dir}" --verify
    RESULT_VARIABLE reuse_status OUTPUT_QUIET ERROR_VARIABLE reuse_error)
if(reuse_status EQUAL 0 OR NOT reuse_error MATCHES "the output directory must be new")
    message(FATAL_ERROR "Expected rejection of an existing output directory\n${reuse_error}")
endif()

file(REMOVE_RECURSE "${trace_dir}")
