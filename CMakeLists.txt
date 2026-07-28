cmake_minimum_required(VERSION 3.16)
project(lowlat_oms_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ---- core library ---------------------------------------------------------
add_library(lloms
    src/order_book.cpp
    src/matching_engine.cpp
    src/risk_gate.cpp
    src/latency_histogram.cpp)
target_include_directories(lloms PUBLIC include)
target_compile_features(lloms PUBLIC cxx_std_17)

# ---- tests ----------------------------------------------------------------
enable_testing()
add_executable(unit_tests
    tests/test_main.cpp
    tests/test_spsc_ring_buffer.cpp
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_risk_gate.cpp
    tests/test_latency_histogram.cpp
    tests/test_config.cpp)
target_include_directories(unit_tests PRIVATE tests)
target_link_libraries(unit_tests PRIVATE lloms Threads::Threads)
add_test(NAME unit_tests COMMAND unit_tests)

# ---- benchmarks -----------------------------------------------------------
add_executable(bench_ring_buffer bench/bench_ring_buffer.cpp)
target_link_libraries(bench_ring_buffer PRIVATE lloms Threads::Threads)

add_executable(bench_matching bench/bench_matching.cpp)
target_link_libraries(bench_matching PRIVATE lloms Threads::Threads)

# ---- Linux-only feed handler (epoll) --------------------------------------
if(UNIX AND NOT APPLE)
    add_executable(feed_handler apps/feed_handler_linux.cpp)
    target_link_libraries(feed_handler PRIVATE lloms Threads::Threads)
endif()
