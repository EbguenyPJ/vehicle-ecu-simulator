cmake_minimum_required(VERSION 3.16)
project(EcuPublicConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ECU_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)

add_subdirectory("${ECU_SOURCE_DIR}" ecu EXCLUDE_FROM_ALL)

add_executable(ecu-public-consumer main.cpp)
target_link_libraries(ecu-public-consumer PRIVATE ecu_simulator ecu_dashboard)
set_target_properties(ecu-public-consumer PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/bin/$<CONFIG>")
