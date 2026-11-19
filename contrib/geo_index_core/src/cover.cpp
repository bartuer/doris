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
    // GetCovering returns a normalized union (sorted, no cell contains another),
    // which the merge below relies on.  No boundary buffer: plan 02 A.4 checked
    // 273,621 inside + 48,416 boundary points with 0 false negatives without it,
    // and P.1 measured it at +18-42% candidates (fix.archive/02.geo.core.bench.md).

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
