cmake_minimum_required(VERSION 3.16)
project(VehicleEcuSimulator LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(ecu_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
    endif()
endfunction()

add_library(ecu_gateway STATIC src/gateway.cpp)
target_compile_features(ecu_gateway PUBLIC cxx_std_17)
target_include_directories(ecu_gateway PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
ecu_enable_warnings(ecu_gateway)

add_library(ecu_scaling STATIC src/scaling.cpp)
target_compile_features(ecu_scaling PUBLIC cxx_std_17)
target_include_directories(ecu_scaling PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
ecu_enable_warnings(ecu_scaling)

add_library(ecu_control STATIC src/control.cpp)
target_link_libraries(ecu_control PUBLIC ecu_gateway)
ecu_enable_warnings(ecu_control)

add_library(ecu_record STATIC src/record.cpp)
target_link_libraries(ecu_record PUBLIC ecu_control)
ecu_enable_warnings(ecu_record)

add_library(ecu_csv STATIC src/csv.cpp)
target_link_libraries(ecu_csv PUBLIC ecu_record)
ecu_enable_warnings(ecu_csv)

add_library(ecu_binary STATIC src/binary.cpp)
target_link_libraries(ecu_binary PUBLIC ecu_record PRIVATE ecu_scaling)
ecu_enable_warnings(ecu_binary)

add_library(ecu_simulator STATIC src/simulator.cpp)
target_link_libraries(ecu_simulator PUBLIC ecu_gateway)
ecu_enable_warnings(ecu_simulator)

add_library(ecu_dashboard STATIC src/braille.cpp src/dashboard.cpp)
target_link_libraries(ecu_dashboard PUBLIC ecu_control)
ecu_enable_warnings(ecu_dashboard)

add_executable(vehicle-ecu-simulator src/main.cpp)
target_link_libraries(vehicle-ecu-simulator PRIVATE ecu_simulator ecu_dashboard ecu_csv ecu_binary)
ecu_enable_warnings(vehicle-ecu-simulator)

option(ECU_BUILD_BENCHMARKS "Build the core, Dashboard and log benchmarks" OFF)
if(ECU_BUILD_BENCHMARKS)
    add_executable(ecu-performance benchmarks/performance.cpp)
    target_link_libraries(ecu-performance PRIVATE ecu_simulator ecu_dashboard)
    ecu_enable_warnings(ecu-performance)

    add_executable(ecu-log-performance benchmarks/log_performance.cpp)
    target_link_libraries(ecu-log-performance PRIVATE ecu_csv ecu_binary ecu_simulator)
    target_include_directories(ecu-log-performance PRIVATE tests)
    ecu_enable_warnings(ecu-log-performance)
endif()

include(CTest)
if(BUILD_TESTING)
    if(ECU_BUILD_BENCHMARKS)
        add_test(NAME benchmark.logs COMMAND "${CMAKE_COMMAND}"
            "-DECU_LOG_BENCHMARK=$<TARGET_FILE:ecu-log-performance>"
            "-DECU_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/log-benchmark/$<CONFIG>"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_log_benchmark.cmake")
        set_tests_properties(benchmark.logs PROPERTIES TIMEOUT 10)
    endif()

    add_executable(binary_tests tests/binary_tests.cpp)
    target_link_libraries(binary_tests PRIVATE ecu_binary ecu_csv ecu_simulator)
    ecu_enable_warnings(binary_tests)

    foreach(test_case IN ITEMS reference_bytes round_trip malformed stream_errors scenarios)
        add_test(NAME binary.${test_case} COMMAND binary_tests ${test_case})
    endforeach()
    add_test(NAME binary.cli COMMAND "${CMAKE_COMMAND}"
        "-DECU_EXECUTABLE=$<TARGET_FILE:vehicle-ecu-simulator>"
        "-DECU_BINARY_TESTS=$<TARGET_FILE:binary_tests>"
        "-DECU_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/binary-cli/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_binary_cli.cmake")
    set_tests_properties(binary.cli PROPERTIES TIMEOUT 10)

    add_executable(csv_tests tests/csv_tests.cpp)
    target_link_libraries(csv_tests PRIVATE ecu_csv ecu_simulator)
    ecu_enable_warnings(csv_tests)

    foreach(test_case IN ITEMS record round_trip malformed stream_errors observer_failure scenarios)
        add_test(NAME csv.${test_case} COMMAND csv_tests ${test_case})
    endforeach()
    add_test(NAME csv.cli COMMAND "${CMAKE_COMMAND}"
        "-DECU_EXECUTABLE=$<TARGET_FILE:vehicle-ecu-simulator>"
        "-DECU_CSV_TESTS=$<TARGET_FILE:csv_tests>"
        "-DECU_TEST_DIR=${CMAKE_CURRENT_BINARY_DIR}/csv-cli/$<CONFIG>"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_csv_cli.cmake")
    set_tests_properties(csv.cli PROPERTIES TIMEOUT 10)

    add_executable(scaling_tests tests/scaling_tests.cpp)
    target_link_libraries(scaling_tests PRIVATE ecu_scaling ecu_gateway)
    ecu_enable_warnings(scaling_tests)

    foreach(test_case IN ITEMS profiles rounding saturation invalid_input round_trip observations)
        add_test(NAME scaling.${test_case} COMMAND scaling_tests ${test_case})
    endforeach()

    add_executable(gateway_tests tests/gateway_tests.cpp)
    target_link_libraries(gateway_tests PRIVATE ecu_gateway)
    ecu_enable_warnings(gateway_tests)

    foreach(test_case IN ITEMS catalog_and_range freshness malformed_batch snapshot cycle_order
                               status configurable_freshness configuration_ownership counts lookup)
        add_test(NAME gateway.${test_case} COMMAND gateway_tests ${test_case})
    endforeach()

    add_executable(control_tests tests/control_tests.cpp)
    target_link_libraries(control_tests PRIVATE ecu_control)
    ecu_enable_warnings(control_tests)

    foreach(test_case IN ITEMS startup degradation operating_limits concurrent_faults safe_latch cycle_order
                               self_test shutdown diagnostic_priority)
        add_test(NAME control.${test_case} COMMAND control_tests ${test_case})
    endforeach()

    add_executable(simulation_tests tests/simulation_tests.cpp)
    target_link_libraries(simulation_tests PRIVATE ecu_simulator ecu_control)
    ecu_enable_warnings(simulation_tests)

    foreach(test_case IN ITEMS nominal fault_schedule integration lifecycle acceptance search_boundaries)
        add_test(NAME simulation.${test_case} COMMAND simulation_tests ${test_case})
        set_tests_properties(simulation.${test_case} PROPERTIES TIMEOUT 10)
    endforeach()

    foreach(scenario IN ITEMS nominal faults battery-fault oil-fault temperature-dropout)
        string(REPLACE "-" "_" test_suffix "${scenario}")
        add_test(NAME simulation.cli_${test_suffix} COMMAND vehicle-ecu-simulator ${scenario})
        set_tests_properties(simulation.cli_${test_suffix} PROPERTIES TIMEOUT 10)
    endforeach()

    add_executable(dashboard_tests tests/dashboard_tests.cpp)
    target_link_libraries(dashboard_tests PRIVATE ecu_dashboard ecu_simulator)
    ecu_enable_warnings(dashboard_tests)

    foreach(test_case IN ITEMS ascii braille pipeline lifecycle summary)
        add_test(NAME dashboard.${test_case} COMMAND dashboard_tests ${test_case})
    endforeach()

    add_test(NAME build.consumer COMMAND "${CMAKE_COMMAND}"
        "-DECU_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
        "-DECU_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/consumer-test/$<CONFIG>"
        "-DECU_GENERATOR=${CMAKE_GENERATOR}"
        "-DECU_GENERATOR_PLATFORM=${CMAKE_GENERATOR_PLATFORM}"
        "-DECU_GENERATOR_TOOLSET=${CMAKE_GENERATOR_TOOLSET}"
        "-DECU_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
        "-DECU_CONFIG=$<CONFIG>"
        "-DECU_EXECUTABLE_SUFFIX=${CMAKE_EXECUTABLE_SUFFIX}"
        -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/check_consumer.cmake")
endif()
