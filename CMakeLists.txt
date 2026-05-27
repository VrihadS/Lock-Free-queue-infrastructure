cmake_minimum_required(VERSION 3.16)
project(spsc_queue LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=thread,undefined")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(spsc_lib INTERFACE)
target_include_directories(spsc_lib INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(spsc_lib INTERFACE -Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

# ── SPSC ──────────────────────────────────────────────────────────────────
add_executable(bench_spsc   bench/bench_spsc.cpp)
add_executable(test_spsc    tests/test_spsc.cpp)
target_link_libraries(bench_spsc  PRIVATE spsc_lib Threads::Threads)
target_link_libraries(test_spsc   PRIVATE spsc_lib Threads::Threads)

# ── NBLFQ (MPMC) ──────────────────────────────────────────────────────────
add_executable(bench_nblfq  bench/bench_nblfq.cpp)
add_executable(test_nblfq   tests/test_nblfq.cpp)
target_link_libraries(bench_nblfq PRIVATE spsc_lib Threads::Threads)
target_link_libraries(test_nblfq  PRIVATE spsc_lib Threads::Threads)

enable_testing()
add_test(NAME spsc_tests  COMMAND test_spsc)
add_test(NAME nblfq_tests COMMAND test_nblfq)

message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
