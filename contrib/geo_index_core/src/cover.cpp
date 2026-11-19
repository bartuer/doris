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


#include "geo_index/cover.h"

#include <s2/s1angle.h>
#include <s2/s2cell_id.h>
#include <s2/s2cell_union.h>
#include <s2/s2polygon.h>
#include <s2/s2region_coverer.h>

#include "check.h"
#include "geo_index/cell.h"

namespace geo_index {

std::vector<CellRange> cover(const S2Polygon& polygon, int level, int max_cells) {
    GEO_CHECK(level >= 0 && level <= S2CellId::kMaxLevel);
    GEO_CHECK(max_cells >= 1);
    S2RegionCoverer::Options options;
    options.set_max_level(level);
    options.set_max_cells(max_cells);
    S2RegionCoverer coverer(options);
    S2CellUnion covering = coverer.GetCovering(polygon);
    // An S2Cell is closed, but S2CellId(point) assigns a boundary point to only ONE of
    // the cells sharing that boundary, so a polygon point on a covering cell's edge can
    // get the neighbour's id (audit 2026-11-19 #1).  Grow the union by a radius > 0:
    // every point of the old union is then strictly inside the new one, so the leaf
    // cell it is assigned to is under a union cell.  max_level_diff bounds the growth
    // (<= 4 * (1 + 2^3) = 36x cells, buffer ~1/8 of the largest cell); Expand
    // normalizes (sorted, no cell contains another), which the merge below relies on.
    // kiss: fixed buffer; upgrade when Z.3 Profile shows the buffer costs > 1% rows.
    covering.Expand(S1Angle::Radians(1e-9), 3);

    // Merge in uint64 S2 order (to_bigint preserves it).  Adjacent level-`level`
    // cells (hi.next() == first) join one range, so fewer BETWEEN terms.
    std::vector<CellRange> ranges;
    S2CellId lo;
    S2CellId hi;
    for (const S2CellId& c : covering) {
        // Every stored level-`level` descendant of c lies in [first, last].
        S2CellId first = c.range_min().parent(level);
        S2CellId last = c.range_max().parent(level);
        // hi.next() of the last cell on the curve is invalid (id past face 5): then
        // nothing can follow, so the plain comparison is still correct.
        if (hi.is_valid() && first <= hi.next()) {
            hi = last;
            continue;
        }
        if (hi.is_valid()) {
            ranges.push_back({to_bigint(lo.id()), to_bigint(hi.id())});
        }
        lo = first;
        hi = last;
    }
    if (hi.is_valid()) {
        ranges.push_back({to_bigint(lo.id()), to_bigint(hi.id())});
    }
    return ranges;
}

} // namespace geo_index
