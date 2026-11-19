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
# Plan 02 P.1: Release build + Google Benchmark + fixture selectivity.
#   Docker: docker run --rm -v "$PWD":/src geo-index-core /src/run_bench.sh
# Writes $BUILD_ROOT/bench/{bench.json,selectivity.tsv}.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD_ROOT="${BUILD_ROOT:-${HERE}/build}"
EXTRA=()
if [[ "$(uname -s)" == "Darwin" ]]; then
    EXTRA+=("-DCMAKE_PREFIX_PATH=$(brew --prefix)")
fi
cmake -S "${HERE}" -B "${BUILD_ROOT}/bench" -DCMAKE_BUILD_TYPE=Release "${EXTRA[@]}" >/dev/null
cmake --build "${BUILD_ROOT}/bench" -j "$(getconf _NPROCESSORS_ONLN)" --target geo_index_core_bench
"${BUILD_ROOT}/bench/geo_index_core_bench" --geo_selectivity | tee "${BUILD_ROOT}/bench/selectivity.tsv"
"${BUILD_ROOT}/bench/geo_index_core_bench" --benchmark_repetitions=3 --benchmark_report_aggregates_only=true \
    --benchmark_out="${BUILD_ROOT}/bench/bench.json" --benchmark_out_format=json
