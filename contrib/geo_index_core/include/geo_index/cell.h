// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#pragma once

#include <cstdint>

class S2CellId;

namespace geo_index {

// Cell ids are S2 cell ids at a fixed level, stored as a signed BIGINT.
// S2 ids are uint64 in Hilbert-curve order; flipping the top bit makes signed
// int64 comparison give the same order, so BIGINT range scans follow the curve.
inline int64_t to_bigint(uint64_t s2_id) {
    return static_cast<int64_t>(s2_id ^ (uint64_t {1} << 63));
}
inline uint64_t to_s2_id(int64_t cell) {
    return static_cast<uint64_t>(cell) ^ (uint64_t {1} << 63);
}

// The S2 cell at `level` (0..30) containing (lat, lon) in degrees, as BIGINT.
int64_t cell_of(double lat, double lon, int level);

} // namespace geo_index
