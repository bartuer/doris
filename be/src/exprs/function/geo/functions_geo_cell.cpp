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

// s2_cell(lat, lng[, level]): the S2 cell id of a point at `level` (default 16)
// as an order-preserving BIGINT, for a generated geo index column (plan 01/02).
// s2_covers(cell, polygon_wkt): true when `cell` is in the polygon's S2 cover --
// an index predicate: a superset of ST_Contains(polygon, point), answered from
// the cell column's BKD index.  The arithmetic lives in contrib/geo_index_core;
// this file is the Doris adapter.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <vector>

#include "core/block/block.h"
#include "core/column/column_execute_util.h"
#include "core/column/column_nullable.h"
#include "core/data_type/data_type_nullable.h"
#include "core/data_type/data_type_number.h"
#include "exprs/function/geo/geo_types.h"
#include "exprs/function/simple_function_factory.h"
#include "geo_index/cell.h"
#include "geo_index/cover.h"
#include "storage/index/index_reader_helper.h"
#include "storage/index/inverted/inverted_index_iterator.h"

namespace doris {

template <bool with_level>
class FunctionS2Cell : public IFunction {
public:
    static constexpr auto name = "s2_cell";
    static FunctionPtr create() { return std::make_shared<FunctionS2Cell>(); }
    String get_name() const override { return name; }
    size_t get_number_of_arguments() const override { return with_level ? 3 : 2; }

    DataTypes get_variadic_argument_types_impl() const override {
        DataTypes types {std::make_shared<DataTypeFloat64>(), std::make_shared<DataTypeFloat64>()};
        if constexpr (with_level) {
            types.push_back(std::make_shared<DataTypeInt32>());
        }
        return types;
    }

    DataTypePtr get_return_type_impl(const DataTypes&) const override {
        return make_nullable(std::make_shared<DataTypeInt64>());
    }

    Status execute_impl(FunctionContext*, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t rows) const override {
        auto lat = ColumnView<TYPE_DOUBLE>::create(block.get_by_position(arguments[0]).column);
        auto lng = ColumnView<TYPE_DOUBLE>::create(block.get_by_position(arguments[1]).column);
        auto values = ColumnInt64::create(rows, 0);
        auto nulls = ColumnUInt8::create(rows, 0);
        auto compute = [&](auto level_at) {
            for (size_t row = 0; row < rows; ++row) {
                const double latitude = lat.value_at(row);
                const double longitude = lng.value_at(row);
                const int level = level_at(row);
                // Same domain rule as h3_lat_lng_to_cell: out-of-range input is NULL.
                if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 ||
                    latitude > 90 || longitude < -180 || longitude > 180 || level < 0 ||
                    level > 30) {
                    nulls->get_data()[row] = 1;
                    continue;
                }
                values->get_data()[row] = geo_index::cell_of(latitude, longitude, level);
            }
        };
        if constexpr (with_level) {
            auto levels = ColumnView<TYPE_INT>::create(block.get_by_position(arguments[2]).column);
            compute([&](size_t row) { return levels.value_at(row); });
        } else {
            compute([](size_t) { return geo_index::kDefaultLevel; });
        }
        block.replace_by_position(result,
                                  ColumnNullable::create(std::move(values), std::move(nulls)));
        return Status::OK();
    }
};

// Ranges of the default-level cell column covering a WKT polygon; empty when the
// text is not a polygon (no cell can be inside it).
static std::vector<geo_index::CellRange> polygon_ranges(const StringRef& wkt) {
    GeoParseStatus status;
    auto shape = GeoShape::from_wkt(wkt.data, wkt.size, status);
    if (shape == nullptr || status != GeoParseStatus::GEO_PARSE_OK ||
        shape->type() != GeoShapeType::GEO_SHAPE_POLYGON) {
        return {};
    }
    // The polygon Doris' ST_Contains uses, so the cover is a superset of its answer.
    const auto* polygon = assert_cast<const GeoPolygon*>(shape.get())->polygon();
    return geo_index::cover(*polygon, geo_index::kDefaultLevel, geo_index::kDefaultMaxCells);
}

class FunctionS2Covers : public IFunction {
public:
    static constexpr auto name = "s2_covers";
    static FunctionPtr create() { return std::make_shared<FunctionS2Covers>(); }
    String get_name() const override { return name; }
    size_t get_number_of_arguments() const override { return 2; }

    DataTypePtr get_return_type_impl(const DataTypes&) const override {
        return std::make_shared<DataTypeUInt8>();
    }

    // branch-4.1 adapter: this branch has no approximate index results, and none is needed:
    // the index answer IS the exact answer of s2_covers (cell in the cover ranges), and the
    // exact geometry test stays in the separate ST_Contains conjunct.  (Master marks it
    // approximate so the BKD bitmap only narrows rows.)  No BETWEEN_QUERY here either: each
    // cover range is (>= lo) AND (<= hi), correct but slower than master (plan 01 C.4).

    Status execute_impl(FunctionContext*, Block& block, const ColumnNumbers& arguments,
                        uint32_t result, size_t rows) const override {
        // (cell, polygon) order, same as master: indexed column first, literal second.
        auto cells = ColumnView<TYPE_BIGINT>::create(block.get_by_position(arguments[0]).column);
        const auto& [polygon_column, polygon_const] =
                unpack_if_const(block.get_by_position(arguments[1]).column);
        const auto* wkt = assert_cast<const ColumnString*>(polygon_column.get());
        auto res = ColumnUInt8::create(rows, 0);
        std::vector<geo_index::CellRange> ranges;
        if (polygon_const) {
            ranges = polygon_ranges(wkt->get_data_at(0));
        }
        for (size_t row = 0; row < rows; ++row) {
            if (!polygon_const) {
                ranges = polygon_ranges(wkt->get_data_at(row));
            }
            const int64_t cell = cells.value_at(row);
            auto it = std::upper_bound(
                    ranges.begin(), ranges.end(), cell,
                    [](int64_t c, const geo_index::CellRange& r) { return c < r.lo; });
            res->get_data()[row] = it != ranges.begin() && cell <= std::prev(it)->hi;
        }
        block.replace_by_position(result, std::move(res));
        return Status::OK();
    }

    // One BKD range query per cover range, unioned.  Called by VExpr with the const
    // polygon literal in `arguments` and the cell column's iterator.
    Status evaluate_inverted_index(
            const ColumnsWithTypeAndName& arguments,
            const std::vector<IndexFieldNameAndTypePair>& data_type_with_names,
            std::vector<segment_v2::IndexIterator*> iterators, uint32_t num_rows,
            const InvertedIndexAnalyzerCtx* /*analyzer_ctx*/,
            segment_v2::InvertedIndexResultBitmap& bitmap_result) const override {
        DORIS_CHECK(arguments.size() == 1);
        DORIS_CHECK(iterators.size() == 1);
        auto* iter = iterators[0];
        if (iter == nullptr) {
            return Status::OK();
        }
        if (!segment_v2::IndexReaderHelper::has_bkd_index(iter)) {
            return Status::Error<ErrorCode::INVERTED_INDEX_EVALUATE_SKIPPED>(
                    "s2_covers needs a BKD (numeric inverted) index on the cell column");
        }
        const auto& [polygon_column, polygon_const] = unpack_if_const(arguments[0].column);
        if (!polygon_const) {
            return Status::Error<ErrorCode::INVERTED_INDEX_EVALUATE_SKIPPED>(
                    "s2_covers uses the index only for a constant polygon");
        }
        const auto ranges = polygon_ranges(
                assert_cast<const ColumnString*>(polygon_column.get())->get_data_at(0));
        const auto& [column_name, column_type] = data_type_with_names[0];
        auto rows = std::make_shared<roaring::Roaring>();
        for (const auto& range : ranges) {
            auto query = [&](segment_v2::InvertedIndexQueryType type, int64_t value) {
                segment_v2::InvertedIndexParam param;
                param.column_name = column_name;
                param.column_type = column_type;
                param.query_type = type;
                param.query_value = Field::create_field<TYPE_BIGINT>(value);
                param.num_rows = num_rows;
                param.roaring = std::make_shared<roaring::Roaring>();
                // Never bypass: a polygon's cover ranges are narrow by construction, and
                // the per-query hit-count guess (inverted_index_skip_threshold) would
                // otherwise drop the index (fix.archive/01.geo.index.reuse.md).
                param.skip_try = true;
                return std::make_pair(iter->read_from_index(&param), param.roaring);
            };
            auto [lo_status, at_least] =
                    query(segment_v2::InvertedIndexQueryType::GREATER_EQUAL_QUERY, range.lo);
            RETURN_IF_ERROR(lo_status);
            auto [hi_status, at_most] =
                    query(segment_v2::InvertedIndexQueryType::LESS_EQUAL_QUERY, range.hi);
            RETURN_IF_ERROR(hi_status);
            *rows |= *at_least & *at_most;
        }
        bitmap_result =
                segment_v2::InvertedIndexResultBitmap(rows, std::make_shared<roaring::Roaring>());
        return Status::OK();
    }
};

void register_function_geo_cell(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionS2Cell<false>>();
    factory.register_function<FunctionS2Cell<true>>();
    factory.register_function<FunctionS2Covers>();
}

} // namespace doris
