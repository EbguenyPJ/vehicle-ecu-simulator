set(configure_command
    "${CMAKE_COMMAND}"
    -S "${ECU_SOURCE_DIR}/tests/cmake_consumer"
    -B "${ECU_BINARY_DIR}"
    -G "${ECU_GENERATOR}"
    "-DECU_SOURCE_DIR:PATH=${ECU_SOURCE_DIR}"
    "-DCMAKE_CXX_COMPILER:FILEPATH=${ECU_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE:STRING=${ECU_CONFIG}")

if(ECU_GENERATOR_PLATFORM)
    list(APPEND configure_command -A "${ECU_GENERATOR_PLATFORM}")
endif()
if(ECU_GENERATOR_TOOLSET)
    list(APPEND configure_command -T "${ECU_GENERATOR_TOOLSET}")
endif()

execute_process(COMMAND ${configure_command}
    RESULT_VARIABLE configure_result
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error)
if(NOT configure_result EQUAL 0)
    message(FATAL_ERROR "Consumer configuration failed\n${configure_output}\n${configure_error}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${ECU_BINARY_DIR}"
    --config "${ECU_CONFIG}" --target ecu-public-consumer
    RESULT_VARIABLE build_result
    OUTPUT_VARIABLE build_output
    ERROR_VARIABLE build_error)
if(NOT build_result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed\n${build_output}\n${build_error}")
endif()

execute_process(COMMAND
    "${ECU_BINARY_DIR}/bin/${ECU_CONFIG}/ecu-public-consumer${ECU_EXECUTABLE_SUFFIX}"
    RESULT_VARIABLE run_result
    OUTPUT_VARIABLE run_output
    ERROR_VARIABLE run_error)
if(NOT run_result EQUAL 0)
    message(FATAL_ERROR "Consumer execution failed\n${run_output}\n${run_error}")
endif()
