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
#include <vector>

class S2Polygon;

namespace geo_index {

// Inclusive range of BIGINT cell ids (see cell.h), all at the column level.
struct CellRange {
    int64_t lo;
    int64_t hi;
};

// Sorted, disjoint, non-adjacent ranges whose union holds the cell_of() of every
// point inside `polygon` (boundary included) at `level`.  A superset: callers
// recheck exactly.  `max_cells` bounds the S2 covering size before the boundary
// expansion below (more cells = tighter, more ranges).
std::vector<CellRange> cover(const S2Polygon& polygon, int level, int max_cells);

} // namespace geo_index
