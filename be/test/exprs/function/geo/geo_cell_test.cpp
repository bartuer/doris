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

#include <fstream>
#include <limits>
#include <string>

#include "core/data_type/data_type_number.h"
#include "exprs/function/function_test_util.h"
#include "geo_index/cell.h"

namespace doris {
using namespace ut_type;

TEST(GeoCellTest, S2CellWithLevel) {
    const InputTypeSet types = {TYPE_DOUBLE, TYPE_DOUBLE, TYPE_INT};
    const DataSet cases = {
            {{39.9042, 116.4074, int32_t(16)}, geo_index::cell_of(39.9042, 116.4074, 16)},
            {{-16.0, 180.0, int32_t(30)}, geo_index::cell_of(-16.0, 180.0, 30)},
            {{90.0, 0.0, int32_t(0)}, geo_index::cell_of(90.0, 0.0, 0)},
            {{91.0, 0.0, int32_t(16)}, Null()},
            {{0.0, -181.0, int32_t(16)}, Null()},
            {{0.0, 0.0, int32_t(-1)}, Null()},
            {{0.0, 0.0, int32_t(31)}, Null()},
            {{std::numeric_limits<double>::quiet_NaN(), 0.0, int32_t(16)}, Null()},
            {{0.0, std::numeric_limits<double>::infinity(), int32_t(16)}, Null()},
            {{Null(), 0.0, int32_t(16)}, Null()},
            {{0.0, Null(), int32_t(16)}, Null()},
            {{0.0, 0.0, Null()}, Null()}};
    check_function_all_arg_comb<DataTypeInt64, true>("s2_cell", types, cases);
}

TEST(GeoCellTest, S2CellDefaultLevelIs16) {
    const InputTypeSet types = {TYPE_DOUBLE, TYPE_DOUBLE};
    const DataSet cases = {{{39.9042, 116.4074}, geo_index::cell_of(39.9042, 116.4074, 16)},
                           {{91.0, 0.0}, Null()},
                           {{Null(), 0.0}, Null()}};
    check_function_all_arg_comb<DataTypeInt64, true>("s2_cell", types, cases);
}

// The SQL function and the core agree on every checked-in fixture point
// (contrib/geo_index_core/test/data/points.csv; lon, lat order there).
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
    EXPECT_TRUE((check_function<DataTypeInt64, true>("s2_cell",
                                                     {TYPE_DOUBLE, TYPE_DOUBLE, TYPE_INT}, cases))
                        .ok());
}

} // namespace doris
