cmake_minimum_required(VERSION 3.16)
project(EcuPortafolio LANGUAGES CXX)

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
target_include_directories(ecu_gateway PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
ecu_enable_warnings(ecu_gateway)

include(CTest)
if(BUILD_TESTING)
    add_executable(gateway_tests tests/gateway_tests.cpp)
    target_link_libraries(gateway_tests PRIVATE ecu_gateway)
    ecu_enable_warnings(gateway_tests)

    foreach(test_case IN ITEMS catalog_and_range freshness malformed_batch snapshot cycle_order)
        add_test(NAME gateway.${test_case} COMMAND gateway_tests ${test_case})
    endforeach()
endif()
