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
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "core/column/column_const.h"
#include "core/column/column_string.h"
#include "core/data_type/data_type_number.h"
#include "core/data_type/data_type_string.h"
#include "exprs/function/function_test_util.h"
#include "exprs/function/geo/geo_types.h"
#include "exprs/function/simple_function_factory.h"
#include "geo_index/cell.h"
#include "geo_index/cover.h"
#include "storage/index/index_iterator.h"
#include "storage/index/inverted/inverted_index_reader.h"

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

namespace {

ColumnPtr const_string(const std::string& value, size_t rows) {
    auto column = ColumnString::create();
    column->insert_value(value);
    return ColumnConst::create(std::move(column), rows);
}

struct FixturePoint {
    double lon;
    double lat;
};

std::vector<FixturePoint> load_fixture_points() {
    std::ifstream in("./contrib/geo_index_core/test/data/points.csv");
    std::string header;
    std::getline(in, header);
    std::vector<FixturePoint> points;
    long id;
    char comma;
    FixturePoint p;
    while (in >> id >> comma >> p.lon >> comma >> p.lat) {
        points.push_back(p);
    }
    return points;
}

// The three plan-01 baseline polygons (fix.archive/01.geo.baseline.md), WKT lon lat.
const std::vector<std::pair<std::string, std::string>> kPolygons = {
        {"city",
         "POLYGON((116.20 39.75, 116.55 39.75, 116.60 39.90, 116.55 40.05, 116.20 40.05, "
         "116.15 39.90, 116.20 39.75))"},
        {"thin", "POLYGON((115.50 39.50, 117.50 40.90, 117.49 40.91, 115.49 39.51, 115.50 39.50))"},
        {"concave",
         "POLYGON((116.00 39.60, 117.00 39.60, 117.00 40.40, 116.80 40.40, 116.80 39.80, "
         "116.20 39.80, 116.20 40.40, 116.00 40.40, 116.00 39.60))"}};

class MockBkdReader : public segment_v2::InvertedIndexReader {
public:
    explicit MockBkdReader(const TabletIndex& meta)
            : segment_v2::InvertedIndexReader(&meta, nullptr) {}
    segment_v2::InvertedIndexReaderType type() override {
        return segment_v2::InvertedIndexReaderType::BKD;
    }
    Status query(const segment_v2::IndexQueryContextPtr&, const std::string&, const Field&,
                 segment_v2::InvertedIndexQueryType, std::shared_ptr<roaring::Roaring>&,
                 const InvertedIndexAnalyzerCtx* = nullptr) override {
        return Status::OK();
    }
    Status try_query(const segment_v2::IndexQueryContextPtr&, const std::string&, const Field&,
                     segment_v2::InvertedIndexQueryType, size_t*) override {
        return Status::OK();
    }
    Status new_iterator(std::unique_ptr<segment_v2::IndexIterator>*) override {
        return Status::OK();
    }
};

// A BKD index over a column of cell values: answers >= / <= by brute force, and
// records that every query asked not to be bypassed (skip_try).
class MockCellIndexIterator : public segment_v2::IndexIterator {
public:
    MockCellIndexIterator(std::shared_ptr<MockBkdReader> reader, std::vector<int64_t> cells)
            : _reader(std::move(reader)), _cells(std::move(cells)) {}
    segment_v2::IndexReaderPtr get_reader(segment_v2::IndexReaderType reader_type) const override {
        return std::get<segment_v2::InvertedIndexReaderType>(reader_type) ==
                               segment_v2::InvertedIndexReaderType::BKD
                       ? _reader
                       : nullptr;
    }
    Status read_from_index(const segment_v2::IndexParam& param) override {
        auto* p = std::get<segment_v2::InvertedIndexParam*>(param);
        all_skip_try = all_skip_try && p->skip_try;
        ++queries;
        const int64_t value = p->query_value.get<TYPE_BIGINT>();
        for (uint32_t row = 0; row < _cells.size(); ++row) {
            bool hit = p->query_type == segment_v2::InvertedIndexQueryType::GREATER_EQUAL_QUERY
                               ? _cells[row] >= value
                               : _cells[row] <= value;
            if (hit) {
                p->roaring->add(row);
            }
        }
        return Status::OK();
    }
    Status read_null_bitmap(segment_v2::InvertedIndexQueryCacheHandle*) override {
        return Status::OK();
    }
    Result<bool> has_null() override { return false; }

    bool all_skip_try = true;
    int queries = 0;

private:
    std::shared_ptr<MockBkdReader> _reader;
    std::vector<int64_t> _cells;
};

} // namespace

// plan 01 C.2 / C.3a: the index path of s2_covers returns a superset of the exact
// ST_Contains rows (no false negatives), the row path returns the same rows as the
// index path, and every BKD query is marked skip_try (never bypassed).
TEST(GeoCellTest, S2CoversIndexIsSupersetOfStContains) {
    const auto points = load_fixture_points();
    ASSERT_GT(points.size(), 9000U);
    std::vector<int64_t> cells;
    for (const auto& p : points) {
        cells.push_back(geo_index::cell_of(p.lat, p.lon, geo_index::kDefaultLevel));
    }
    auto cell_column = ColumnInt64::create();
    cell_column->get_data().assign(cells.begin(), cells.end());

    TabletIndex index_meta;
    auto reader = std::make_shared<MockBkdReader>(index_meta);
    auto function = SimpleFunctionFactory::instance().get_function(
            "s2_covers",
            {{nullptr, std::make_shared<DataTypeInt64>(), "cell"},
             {nullptr, std::make_shared<DataTypeString>(), "polygon"}},
            std::make_shared<DataTypeUInt8>());
    ASSERT_NE(function, nullptr);

    for (const auto& [name, wkt] : kPolygons) {
        GeoParseStatus status;
        auto shape = GeoShape::from_wkt(wkt.data(), wkt.size(), status);
        ASSERT_EQ(status, GeoParseStatus::GEO_PARSE_OK) << name;

        // Index path.
        MockCellIndexIterator iter(reader, cells);
        ColumnsWithTypeAndName arguments = {
                {const_string(wkt, 1), std::make_shared<DataTypeString>(), "polygon"}};
        std::vector<IndexFieldNameAndTypePair> fields = {
                {"geo_cell", std::make_shared<DataTypeInt64>()}};
        segment_v2::InvertedIndexResultBitmap bitmap;
        ASSERT_TRUE(function->evaluate_inverted_index(arguments, fields, {&iter},
                                                      static_cast<uint32_t>(cells.size()), nullptr,
                                                      bitmap)
                            .ok())
                << name;
        EXPECT_TRUE(iter.all_skip_try) << name;
        EXPECT_GT(iter.queries, 0) << name;
        const auto& rows = *bitmap.get_data_bitmap();

        // Row path over the same cells.
        Block block;
        block.insert({cell_column->clone(), std::make_shared<DataTypeInt64>(), "cell"});
        block.insert(
                {const_string(wkt, cells.size()), std::make_shared<DataTypeString>(), "polygon"});
        block.insert({nullptr, std::make_shared<DataTypeUInt8>(), "result"});
        FunctionUtils utils(std::make_shared<DataTypeUInt8>(),
                            {std::make_shared<DataTypeInt64>(), std::make_shared<DataTypeString>()},
                            false);
        ASSERT_TRUE(function->execute(utils.get_fn_ctx(), block, {0, 1}, 2, cells.size()).ok());
        const auto& row_result =
                assert_cast<const ColumnUInt8&>(*block.get_by_position(2).column).get_data();

        long hits = 0;
        for (uint32_t row = 0; row < points.size(); ++row) {
            GeoPoint point;
            ASSERT_EQ(point.from_coord(points[row].lon, points[row].lat),
                      GeoParseStatus::GEO_PARSE_OK);
            const bool inside = shape->contains(&point);
            hits += inside;
            ASSERT_TRUE(!inside || rows.contains(row))
                    << name << ": ST_Contains row " << row << " missing from the index result";
            ASSERT_EQ(rows.contains(row), row_result[row] == 1) << name << " row " << row;
        }
        EXPECT_GT(hits, 0) << name;
        EXPECT_LE(rows.cardinality(), points.size()) << name;
    }
}

} // namespace doris
