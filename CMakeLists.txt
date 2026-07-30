cmake_minimum_required(VERSION 3.16)
project(lowlat_oms_core CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

if(MSVC)
    # C4324 fires on every alignas(64) member: "structure was padded due to
    # alignment specifier". That padding is the entire point here -- it is what
    # keeps a producer index and a consumer index off the same cache line -- so
    # the warning reports the design working, not a defect.
    add_compile_options(/W4 /permissive- /wd4324)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

find_package(Threads REQUIRED)

# ---- core library ---------------------------------------------------------
add_library(lloms
    src/order_book.cpp
    src/matching_engine.cpp
    src/risk_gate.cpp
    src/latency_histogram.cpp
    src/wire.cpp
    src/session.cpp
    src/order_router.cpp
    src/cpu.cpp
    src/posix_shm.cpp)
target_include_directories(lloms PUBLIC include)
target_compile_features(lloms PUBLIC cxx_std_17)
# Thread affinity needs pthread; shared memory needs librt on older glibc
# (shm_open moved into libc in glibc 2.34, so treat rt as optional).
target_link_libraries(lloms PUBLIC Threads::Threads)
if(UNIX AND NOT APPLE)
    find_library(LLOMS_RT_LIB rt)
    if(LLOMS_RT_LIB)
        target_link_libraries(lloms PUBLIC ${LLOMS_RT_LIB})
    endif()
endif()

# ---- tests ----------------------------------------------------------------
enable_testing()
add_executable(unit_tests
    tests/test_main.cpp
    tests/test_spsc_ring_buffer.cpp
    tests/test_order_book.cpp
    tests/test_matching_engine.cpp
    tests/test_risk_gate.cpp
    tests/test_latency_histogram.cpp
    tests/test_config.cpp
    tests/test_wire.cpp
    tests/test_session.cpp
    tests/test_shm_ring.cpp
    tests/test_order_router.cpp)
target_include_directories(unit_tests PRIVATE tests)
target_link_libraries(unit_tests PRIVATE lloms Threads::Threads)
add_test(NAME unit_tests COMMAND unit_tests)

# ---- benchmarks -----------------------------------------------------------
add_executable(bench_ring_buffer bench/bench_ring_buffer.cpp)
target_link_libraries(bench_ring_buffer PRIVATE lloms Threads::Threads)

add_executable(bench_pingpong bench/bench_pingpong.cpp)
target_link_libraries(bench_pingpong PRIVATE lloms Threads::Threads)

add_executable(bench_matching bench/bench_matching.cpp)
target_link_libraries(bench_matching PRIVATE lloms Threads::Threads)

# ---- POSIX-only apps ------------------------------------------------------
if(UNIX AND NOT APPLE)
    # epoll feed handler: network thread -> ring buffer -> risk + matching
    add_executable(feed_handler apps/feed_handler_linux.cpp)
    target_link_libraries(feed_handler PRIVATE lloms Threads::Threads)

    # io_uring feed handler: same pipeline, io_uring instead of epoll on the
    # network thread. Built only when liburing is actually present -- the
    # source file itself degrades to a stub without it (see the file header),
    # so this find is an optimization (skip a doomed compile) rather than a
    # correctness requirement.
    find_path(LLOMS_URING_INCLUDE_DIR liburing.h)
    find_library(LLOMS_URING_LIB uring)
    if(LLOMS_URING_INCLUDE_DIR AND LLOMS_URING_LIB)
        add_executable(feed_handler_iouring apps/feed_handler_iouring_linux.cpp)
        target_include_directories(feed_handler_iouring PRIVATE ${LLOMS_URING_INCLUDE_DIR})
        target_link_libraries(feed_handler_iouring PRIVATE lloms Threads::Threads ${LLOMS_URING_LIB})
    else()
        message(STATUS "liburing not found -- feed_handler_iouring target skipped "
                        "(install liburing-dev to build it)")
    endif()
endif()

if(UNIX)
    # two processes, one shared-memory segment, two lock-free rings
    add_executable(shm_ipc apps/shm_ipc_posix.cpp)
    target_link_libraries(shm_ipc PRIVATE lloms Threads::Threads)
endif()
