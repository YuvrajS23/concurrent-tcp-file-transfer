cmake_minimum_required(VERSION 3.20)

project(concurrent_tcp_file_transfer VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(tcp_file_server src/server.cpp)
add_executable(tcp_file_client src/client.cpp)

foreach(target tcp_file_server tcp_file_client)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
endforeach()

include(CTest)
if(BUILD_TESTING)
    add_test(
        NAME tcp_file_transfer_integration
        COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration_test.sh"
                "$<TARGET_FILE:tcp_file_server>" "$<TARGET_FILE:tcp_file_client>")
    set_tests_properties(tcp_file_transfer_integration PROPERTIES TIMEOUT 60)
endif()
