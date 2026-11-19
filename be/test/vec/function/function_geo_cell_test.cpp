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

// branch-3.1 port of master's be/test/exprs/function/geo/geo_cell_test.cpp: the SQL
// functions agree with contrib/geo_index_core on known points and on all checked-in
// fixtures, and s2_covers' row path is a superset of ST_Contains.  (The index path is
// covered on the cluster by regression-test/suites/geo_index_p0.)

#include <gtest/gtest.h>

#include <fstream>
#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "function_test_util.h"
#include "geo/geo_common.h"
#include "geo/geo_types.h"
#include "geo_index/cell.h"
#include "geo_index/cover.h"
#include "vec/core/types.h"

namespace doris::vectorized {

TEST(GeoCellTest, S2CellWithLevel) {
    InputTypeSet types = {TypeIndex::Float64, TypeIndex::Float64, TypeIndex::Int32};
    DataSet cases = {{{39.9042, 116.4074, int32_t(16)}, geo_index::cell_of(39.9042, 116.4074, 16)},
                     {{-16.0, 180.0, int32_t(30)}, geo_index::cell_of(-16.0, 180.0, 30)},
                     {{90.0, 0.0, int32_t(0)}, geo_index::cell_of(90.0, 0.0, 0)},
                     {{91.0, 0.0, int32_t(16)}, Null()},
                     {{0.0, -181.0, int32_t(16)}, Null()},
                     {{0.0, 0.0, int32_t(-1)}, Null()},
                     {{0.0, 0.0, int32_t(31)}, Null()},
                     {{std::numeric_limits<double>::quiet_NaN(), 0.0, int32_t(16)}, Null()},
                     {{Null(), 0.0, int32_t(16)}, Null()},
                     {{0.0, 0.0, Null()}, Null()}};
    EXPECT_TRUE((check_function<DataTypeInt64, true>("s2_cell", types, cases)).ok());
}

TEST(GeoCellTest, S2CellDefaultLevelIs16) {
    InputTypeSet types = {TypeIndex::Float64, TypeIndex::Float64};
    DataSet cases = {{{39.9042, 116.4074}, geo_index::cell_of(39.9042, 116.4074, 16)},
                     {{91.0, 0.0}, Null()},
                     {{Null(), 0.0}, Null()}};
    EXPECT_TRUE((check_function<DataTypeInt64, true>("s2_cell", types, cases)).ok());
}

TEST(GeoCellTest, S2CellMatchesCoreOnFixtures) {
    std::ifstream in("./contrib/geo_index_core/test/data/points.csv");
    ASSERT_TRUE(in.good());
    std::string header;
    std::getline(in, header);
    DataSet cases;
    long id;
    char comma;
    double lon;
    double lat;
    while (in >> id >> comma >> lon >> comma >> lat) {
        cases.push_back({{lat, lon, int32_t(16)}, geo_index::cell_of(lat, lon, 16)});
    }
    ASSERT_GT(cases.size(), 9000U);
    EXPECT_TRUE(
            (check_function<DataTypeInt64, true>(
                     "s2_cell", {TypeIndex::Float64, TypeIndex::Float64, TypeIndex::Int32}, cases))
                    .ok());
}

// Row path of s2_covers: every fixture point ST_Contains accepts is covered.
TEST(GeoCellTest, S2CoversRowPathIsSupersetOfStContains) {
    const std::string city =
            "POLYGON((116.20 39.75, 116.55 39.75, 116.60 39.90, 116.55 40.05, 116.20 40.05, "
            "116.15 39.90, 116.20 39.75))";
    GeoParseStatus status;
    std::unique_ptr<GeoShape> shape(GeoShape::from_wkt(city.data(), city.size(), &status));
    ASSERT_EQ(status, GEO_PARSE_OK);

    std::ifstream in("./contrib/geo_index_core/test/data/points.csv");
    std::string header;
    std::getline(in, header);
    DataSet cases;
    long id;
    char comma;
    double lon;
    double lat;
    int inside_count = 0;
    while (in >> id >> comma >> lon >> comma >> lat) {
        GeoPoint point;
        ASSERT_EQ(point.from_coord(lon, lat), GEO_PARSE_OK);
        if (!shape->contains(&point)) {
            continue;
        }
        ++inside_count;
        cases.push_back({{geo_index::cell_of(lat, lon, 16), city}, uint8_t(1)});
    }
    ASSERT_GT(inside_count, 0);
    EXPECT_TRUE((check_function<DataTypeUInt8, false>(
                         "s2_covers", {TypeIndex::Int64, Consted {TypeIndex::String}}, cases))
                        .ok());
}

} // namespace doris::vectorized
