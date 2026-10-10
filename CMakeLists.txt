cmake_minimum_required(VERSION 3.16)
project(lockfree_queues LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_CXX_FLAGS_RELEASE "-O3 -march=native -mtune=native -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -march=native -g -fno-omit-frame-pointer")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g -fsanitize=thread,undefined")

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

add_library(lfq_headers INTERFACE)
target_include_directories(lfq_headers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(lfq_headers INTERFACE -Wall -Wextra -Wno-unused-parameter)

find_package(Threads REQUIRED)

# ── SPSC ──────────────────────────────────────────────────────────────────
add_executable(bench_spsc   bench/bench_spsc.cpp)
add_executable(test_spsc    tests/test_spsc.cpp)
target_link_libraries(bench_spsc  PRIVATE lfq_headers Threads::Threads)
target_link_libraries(test_spsc   PRIVATE lfq_headers Threads::Threads)

# ── NBLFQ (MPMC) ──────────────────────────────────────────────────────────
add_executable(bench_nblfq  bench/bench_nblfq.cpp)
add_executable(test_nblfq   tests/test_nblfq.cpp)
target_link_libraries(bench_nblfq PRIVATE lfq_headers Threads::Threads)
target_link_libraries(test_nblfq  PRIVATE lfq_headers Threads::Threads)

# ── Optional: benchmark against folly::MPMCQueue ──────────────────────────
# Current folly requires C++20, so only bench_nblfq is raised to C++20.
#
# Option A (Linux server): folly installed to a prefix (getdeps/vcpkg/apt)
#   cmake -B build -DWITH_FOLLY=ON -DCMAKE_PREFIX_PATH=/path/to/folly/install
#
# Option B (in-tree build, e.g. the macOS dev box): point at source + build dirs
#   cmake -B build -DFOLLY_SRC_DIR=~/Developer/folly/folly \
#                  -DFOLLY_BUILD_DIR=~/Developer/folly/folly/_build
option(WITH_FOLLY "Benchmark against folly::MPMCQueue (installed folly, found via find_package)" OFF)
set(FOLLY_SRC_DIR   "" CACHE PATH "folly source checkout (in-tree build, option B)")
set(FOLLY_BUILD_DIR "" CACHE PATH "folly build dir with libfolly.a and folly/folly-config.h (option B)")

if(WITH_FOLLY)
    # An installed folly's exported targets reference glog::glog, and its config only
    # defines that target if the system glog ships a CMake config (Rocky/RHEL's often
    # does not). Create it from the plain library + header in that case.
    if(NOT TARGET glog::glog)
        find_package(glog CONFIG QUIET)
    endif()
    if(NOT TARGET glog::glog)
        find_library(GLOG_LIBRARY glog REQUIRED)
        find_path(GLOG_INCLUDE_DIR glog/logging.h REQUIRED)
        add_library(glog::glog UNKNOWN IMPORTED)
        set_target_properties(glog::glog PROPERTIES
            IMPORTED_LOCATION "${GLOG_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GLOG_INCLUDE_DIR}")
        # glog >= 0.7 headers need this define (they ship glog/export.h)
        if(EXISTS "${GLOG_INCLUDE_DIR}/glog/export.h")
            set_property(TARGET glog::glog APPEND PROPERTY
                INTERFACE_COMPILE_DEFINITIONS GLOG_USE_GLOG_EXPORT)
        endif()
        message(STATUS "glog::glog : created from ${GLOG_LIBRARY}")
    endif()
    # Same story for gflags: folly's link line names the bare target "gflags_shared"
    # (taken from the system gflags-config.cmake), which only exists here if we
    # load that config ourselves.
    if(NOT TARGET gflags_shared)
        find_package(gflags CONFIG QUIET)
    endif()
    if(NOT TARGET gflags_shared)
        find_library(GFLAGS_LIBRARY gflags REQUIRED)
        find_path(GFLAGS_INCLUDE_DIR gflags/gflags.h REQUIRED)
        add_library(gflags_shared UNKNOWN IMPORTED)
        set_target_properties(gflags_shared PROPERTIES
            IMPORTED_LOCATION "${GFLAGS_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${GFLAGS_INCLUDE_DIR}")
        message(STATUS "gflags_shared : created from ${GFLAGS_LIBRARY}")
    endif()
    find_package(folly CONFIG REQUIRED)
    set_target_properties(bench_nblfq PROPERTIES CXX_STANDARD 20)
    target_compile_definitions(bench_nblfq PRIVATE HAVE_FOLLY)
    target_link_libraries(bench_nblfq PRIVATE Folly::folly)
    message(STATUS "folly      : enabled via find_package")
elseif(FOLLY_SRC_DIR AND FOLLY_BUILD_DIR)
    list(PREPEND CMAKE_PREFIX_PATH /opt/homebrew)
    set(_folly_deps glog gflags fmt double-conversion event ssl crypto zstd lz4 snappy sodium)
    set(_folly_dep_libs "")
    foreach(dep ${_folly_deps})
        find_library(_lib_${dep} ${dep} REQUIRED)
        list(APPEND _folly_dep_libs ${_lib_${dep}})
    endforeach()
    find_library(FOLLY_LIB folly PATHS ${FOLLY_BUILD_DIR} NO_DEFAULT_PATH REQUIRED)
    find_path(FOLLY_DEPS_INCLUDE glog/logging.h REQUIRED)

    set_target_properties(bench_nblfq PROPERTIES CXX_STANDARD 20)
    target_compile_definitions(bench_nblfq PRIVATE
        HAVE_FOLLY FMT_SHARED GLOG_USE_GLOG_EXPORT GFLAGS_IS_A_DLL=0)
    target_include_directories(bench_nblfq SYSTEM PRIVATE
        ${FOLLY_SRC_DIR} ${FOLLY_BUILD_DIR} ${FOLLY_DEPS_INCLUDE})
    target_link_libraries(bench_nblfq PRIVATE ${FOLLY_LIB} ${_folly_dep_libs} z c++abi)
    message(STATUS "folly      : enabled in-tree (${FOLLY_LIB})")
endif()

enable_testing()
add_test(NAME spsc_tests  COMMAND test_spsc)
add_test(NAME nblfq_tests COMMAND test_nblfq)

message(STATUS "Build type : ${CMAKE_BUILD_TYPE}")
message(STATUS "Compiler   : ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
