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

#include "geo_index/cell.h"

#include <gtest/gtest.h>
#include <s2/s2cell_id.h>
#include <s2/s2latlng.h>

#include <algorithm>
#include <cstdint>
#include <random>

namespace geo_index {

TEST(GeoIndexCell, BigintMappingKeepsS2Order) {
    std::mt19937_64 rng(42);
    for (int i = 0; i < 100000; ++i) {
        uint64_t a = rng();
        uint64_t b = rng();
        EXPECT_EQ(a < b, to_bigint(a) < to_bigint(b));
        EXPECT_EQ(a, to_s2_id(to_bigint(a)));
    }
    EXPECT_LT(to_bigint(0), to_bigint(~uint64_t {0}));
}

TEST(GeoIndexCell, MatchesS2ParentAtLevel) {
    for (int level : {0, 10, 16, 30}) {
        int64_t cell = cell_of(39.9042, 116.4074, level);
        S2CellId id(to_s2_id(cell));
        ASSERT_TRUE(id.is_valid());
        EXPECT_EQ(level, id.level());
        EXPECT_TRUE(id.contains(S2CellId(S2LatLng::FromDegrees(39.9042, 116.4074))));
    }
}

TEST(GeoIndexCell, AntimeridianAndPoles) {
    // lon 180 and -180 are the same meridian, but at lat -16 it is an S2 cell edge, so
    // rounding may put the two spellings in adjacent leaf cells: equal or edge neighbours.
    S2CellId east(to_s2_id(cell_of(-16.0, 180.0, 30)));
    S2CellId west(to_s2_id(cell_of(-16.0, -180.0, 30)));
    S2CellId neighbours[4];
    east.GetEdgeNeighbors(neighbours);
    EXPECT_TRUE(east == west || std::find(neighbours, neighbours + 4, west) != neighbours + 4);
    // Every longitude at a pole lands in one leaf cell's neighbourhood: same level-10 cell.
    EXPECT_EQ(cell_of(90.0, 0.0, 10), cell_of(90.0, 123.0, 10));
}

TEST(GeoIndexCellDeathTest, LevelOutOfRangeAborts) {
    EXPECT_DEATH(cell_of(0, 0, 31), "GEO_CHECK");
    EXPECT_DEATH(cell_of(0, 0, -1), "GEO_CHECK");
}

} // namespace geo_index
