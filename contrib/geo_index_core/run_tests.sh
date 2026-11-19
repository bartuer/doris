#!/usr/bin/env bash
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
#
# Build and test geo_index_core twice: ASan+UBSan, then plain under valgrind.
#
#   Linux:  apt install cmake g++ libs2-dev libgtest-dev valgrind; ./run_tests.sh
#   macOS:  brew install cmake s2geometry googletest; ./run_tests.sh   (UBSan only; no ASan/valgrind)
#   Docker: docker build -t geo-index-core . && docker run --rm -v "$PWD":/src geo-index-core
#
# BUILD_ROOT (default: ./build) holds the two build trees.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${HERE}/build}"
JOBS="$(getconf _NPROCESSORS_ONLN)"
EXTRA=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    EXTRA+=("-DCMAKE_PREFIX_PATH=$(brew --prefix)")
fi

# LeakSanitizer is not in Apple Clang's ASan (audit #4); CMake also drops ASan on Clang.
LEAKS=1
if [[ "$(uname -s)" == "Darwin" ]]; then
    LEAKS=0
fi

echo "== sanitizers (GCC: ASan + UBSan, Clang: UBSan)"
cmake -S "${HERE}" -B "${BUILD_ROOT}/asan" -DCMAKE_BUILD_TYPE=Debug -DGEO_SANITIZE=ON "${EXTRA[@]}" >/dev/null
cmake --build "${BUILD_ROOT}/asan" -j "${JOBS}"
ASAN_OPTIONS="detect_leaks=${LEAKS}" ctest --test-dir "${BUILD_ROOT}/asan" --output-on-failure -j "${JOBS}"

if [[ "$(uname -s)" == "Darwin" ]]; then
    echo "== valgrind skipped (not available on macOS)"
    exit 0
fi

echo "== valgrind"
cmake -S "${HERE}" -B "${BUILD_ROOT}/plain" -DCMAKE_BUILD_TYPE=RelWithDebInfo "${EXTRA[@]}" >/dev/null
cmake --build "${BUILD_ROOT}/plain" -j "${JOBS}"
# Death tests fork a child that aborts on purpose; valgrind checks the parent run.
valgrind --error-exitcode=1 --leak-check=full --errors-for-leak-kinds=definite,indirect \
    "${BUILD_ROOT}/plain/geo_index_core_test" --gtest_filter='-*DeathTest*'
echo "== all clean"
