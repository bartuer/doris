# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.

# geo_index_core: S2 cell ids + polygon covers for the Doris geo index.
# Standalone (no Doris headers).  Inside Doris, the BE adds this directory and
# links geo_index_core; standalone, this file also builds the gtest suite.
cmake_minimum_required(VERSION 3.16)
project(geo_index_core CXX)

if(NOT CMAKE_CXX_STANDARD)
    set(CMAKE_CXX_STANDARD 17)
endif()
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(GEO_STANDALONE OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(GEO_STANDALONE ON)
endif()

if(GEO_STANDALONE)
    # Sanitizer flags are directory options, set BEFORE any target so the core
    # library itself is instrumented (audit 2026-11-19 #2).
    option(GEO_SANITIZE "Build with sanitizers" OFF)
    if(GEO_SANITIZE)
        if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
            # The system S2/absl libs are built without ASan.  absl changes its btree
            # layout when it detects ASan; under GCC that detection is only the
            # __SANITIZE_ADDRESS__ macro, so undefining it keeps our inline absl code
            # ABI-equal to libs2 while ASan instrumentation stays on.
            add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                                -fno-sanitize-recover=all -U__SANITIZE_ADDRESS__)
            add_link_options(-fsanitize=address,undefined)
        else()
            # Clang (macOS): absl also detects ASan via __has_feature, which cannot be
            # turned off, so ASan against non-ASan S2/absl libs is an ABI mismatch
            # (audit #3).  UBSan only; ASan + valgrind run in the Linux/Docker env.
            message(STATUS "geo_index_core: ${CMAKE_CXX_COMPILER_ID} -> UBSan only (ASan needs GCC here)")
            add_compile_options(-fsanitize=undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)
            add_link_options(-fsanitize=undefined)
        endif()
    endif()

endif()

# The library: the same target standalone and inside the Doris BE build (there
# the BE links it and provides the S2/absl include paths).
add_library(geo_index_core STATIC src/cell.cpp src/cover.cpp)
target_include_directories(geo_index_core PUBLIC include PRIVATE src)

if(GEO_STANDALONE)
    # Find S2 ourselves.  S2 0.10 installs no CMake config, so locate header +
    # library directly; its Abseil dependency has one.
    target_compile_options(geo_index_core PRIVATE -Wall -Wextra -Werror)
    find_path(S2_INCLUDE_DIR s2/s2cell_id.h REQUIRED)
    find_library(S2_LIBRARY s2 REQUIRED)
    find_package(absl REQUIRED)
    find_package(GTest REQUIRED)
    target_include_directories(geo_index_core SYSTEM PUBLIC ${S2_INCLUDE_DIR})
    target_link_libraries(geo_index_core PUBLIC ${S2_LIBRARY} absl::base absl::strings absl::log_severity)

    enable_testing()
    add_executable(geo_index_core_test test/cell_test.cpp test/cover_test.cpp test/property_test.cpp)
    target_compile_options(geo_index_core_test PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(geo_index_core_test PRIVATE
        GEO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
    target_link_libraries(geo_index_core_test PRIVATE geo_index_core GTest::gtest_main)
    include(GoogleTest)
    gtest_discover_tests(geo_index_core_test)

    # Perf (plan 02 P.1): optional, built when Google Benchmark is installed.
    find_package(benchmark QUIET)
    if(benchmark_FOUND)
        add_executable(geo_index_core_bench test/bench_cover.cpp)
        target_compile_options(geo_index_core_bench PRIVATE -Wall -Wextra -Werror)
        target_compile_definitions(geo_index_core_bench PRIVATE
            GEO_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
        target_link_libraries(geo_index_core_bench PRIVATE geo_index_core benchmark::benchmark)
    endif()

    # Own binary: it interposes glibc malloc & co. to count allocations, which
    # clashes with ASan's own interceptors -> plain Linux builds only (run_tests.sh
    # runs it in the valgrind pass).
    if(NOT GEO_SANITIZE AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        add_executable(geo_index_core_alloc_test test/alloc_test.cpp)
        target_compile_options(geo_index_core_alloc_test PRIVATE -Wall -Wextra -Werror)
        target_link_libraries(geo_index_core_alloc_test PRIVATE geo_index_core)
        add_test(NAME GeoIndexCell.CellOfDoesNotAllocate COMMAND geo_index_core_alloc_test)
    endif()
endif()
