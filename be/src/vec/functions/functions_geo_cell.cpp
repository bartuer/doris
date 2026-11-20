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

// branch-3.1 adapter of the geo index functions (plan 01 F2 / Z.5).  The arithmetic is
// contrib/geo_index_core, byte-identical to master.  This file differs from master's
// be/src/exprs/function/geo/functions_geo_cell.cpp only in the Doris 3.1 APIs it uses:
// vectorized:: columns, InvertedIndexIterator::read_from_inverted_index with
// InvertedIndexQueryParamFactory, and (like 4.1) an EXACT index result with one-sided
// BKD queries per cover range (3.1 has neither approximate results nor BETWEEN_QUERY).
//
// s2_cell(lat, lng[, level]): S2 cell id of a point (level default 16) as BIGINT.
// s2_covers(cell, polygon_wkt): true when `cell` is in the polygon's S2 cover.

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <tuple>
#include <vector>

#include "geo/geo_types.h"
#include "geo_index/cell.h"
#include "geo_index/cover.h"
#include "olap/rowset/segment_v2/inverted_index_reader.h"
#include "vec/columns/column_const.h"
#include "vec/columns/column_nullable.h"
#include "vec/columns/column_string.h"
#include "vec/columns/columns_number.h"
#include "vec/common/assert_cast.h"
#include "vec/core/block.h"
#include "vec/data_types/data_type_nullable.h"
#include "vec/data_types/data_type_number.h"
#include "vec/data_types/data_type_string.h"
#include "vec/functions/function.h"
#include "vec/functions/simple_function_factory.h"

namespace doris::vectorized {

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
                        size_t result, size_t rows) const override {
        // Default NULL handling (like h3_lat_lng_to_cell): NULL in -> NULL out before this runs.
        auto values = ColumnInt64::create(rows, 0);
        auto nulls = ColumnUInt8::create(rows, 0);
        auto [lat_column, lat_const] = unpack_if_const(block.get_by_position(arguments[0]).column);
        auto [lng_column, lng_const] = unpack_if_const(block.get_by_position(arguments[1]).column);
        const auto& lat = assert_cast<const ColumnFloat64&>(*lat_column).get_data();
        const auto& lng = assert_cast<const ColumnFloat64&>(*lng_column).get_data();
        ColumnPtr level_column;
        bool level_const = true;
        if constexpr (with_level) {
            std::tie(level_column, level_const) =
                    unpack_if_const(block.get_by_position(arguments[2]).column);
        }
        for (size_t row = 0; row < rows; ++row) {
            const double latitude = lat[index_check_const(row, lat_const)];
            const double longitude = lng[index_check_const(row, lng_const)];
            int level = geo_index::kDefaultLevel;
            if constexpr (with_level) {
                level = assert_cast<const ColumnInt32&>(*level_column)
                                .get_data()[index_check_const(row, level_const)];
            }
            // Same domain rule as master: out-of-range input is NULL.
            if (!std::isfinite(latitude) || !std::isfinite(longitude) || latitude < -90 ||
                latitude > 90 || longitude < -180 || longitude > 180 || level < 0 || level > 30) {
                nulls->get_data()[row] = 1;
                continue;
            }
            values->get_data()[row] = geo_index::cell_of(latitude, longitude, level);
        }
        block.replace_by_position(result,
                                  ColumnNullable::create(std::move(values), std::move(nulls)));
        return Status::OK();
    }
};

// Ranges of the default-level cell column covering a WKT polygon; empty when the text is
// not a polygon.  Uses the polygon Doris' ST_Contains uses.
static std::vector<geo_index::CellRange> polygon_ranges(const StringRef& wkt) {
    GeoParseStatus status;
    std::unique_ptr<GeoShape> shape(GeoShape::from_wkt(wkt.data, wkt.size, &status));
    if (shape == nullptr || status != GEO_PARSE_OK || shape->type() != GEO_SHAPE_POLYGON) {
        return {};
    }
    const auto* polygon = assert_cast<const GeoPolygon*>(shape.get())->polygon();
    return geo_index::cover(*polygon, geo_index::kDefaultLevel, geo_index::kDefaultMaxCells);
}

static bool in_ranges(const std::vector<geo_index::CellRange>& ranges, int64_t cell) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), cell,
                               [](int64_t c, const geo_index::CellRange& r) { return c < r.lo; });
    return it != ranges.begin() && cell <= std::prev(it)->hi;
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

    Status execute_impl(FunctionContext*, Block& block, const ColumnNumbers& arguments,
                        size_t result, size_t rows) const override {
        auto [cell_column, cell_const] =
                unpack_if_const(block.get_by_position(arguments[0]).column);
        auto [wkt_column, wkt_const] = unpack_if_const(block.get_by_position(arguments[1]).column);
        const auto& cells = assert_cast<const ColumnInt64&>(*cell_column).get_data();
        const auto* wkt = assert_cast<const ColumnString*>(wkt_column.get());
        auto res = ColumnUInt8::create(rows, 0);
        std::vector<geo_index::CellRange> ranges;
        if (wkt_const) {
            ranges = polygon_ranges(wkt->get_data_at(0));
        }
        for (size_t row = 0; row < rows; ++row) {
            if (!wkt_const) {
                ranges = polygon_ranges(wkt->get_data_at(row));
            }
            res->get_data()[row] = in_ranges(ranges, cells[index_check_const(row, cell_const)]);
        }
        block.replace_by_position(result, std::move(res));
        return Status::OK();
    }

    // Exact answer of s2_covers from the cell column's BKD index: one (>= lo) AND (<= hi)
    // pair per cover range, unioned.  ST_Contains stays a separate exact conjunct.
    Status evaluate_inverted_index(
            const ColumnsWithTypeAndName& arguments,
            const std::vector<vectorized::IndexFieldNameAndTypePair>& data_type_with_names,
            std::vector<segment_v2::InvertedIndexIterator*> iterators, uint32_t num_rows,
            segment_v2::InvertedIndexResultBitmap& bitmap_result) const override {
        DCHECK(arguments.size() == 1);
        DCHECK(iterators.size() == 1);
        auto* iter = iterators[0];
        if (iter == nullptr) {
            return Status::OK();
        }
        if (iter->get_reader(segment_v2::InvertedIndexReaderType::BKD) == nullptr) {
            return Status::Error<ErrorCode::INVERTED_INDEX_EVALUATE_SKIPPED>(
                    "s2_covers needs a BKD (numeric inverted) index on the cell column");
        }
        auto [wkt_column, wkt_const] = unpack_if_const(arguments[0].column);
        if (!wkt_const) {
            return Status::Error<ErrorCode::INVERTED_INDEX_EVALUATE_SKIPPED>(
                    "s2_covers uses the index only for a constant polygon");
        }
        const auto ranges =
                polygon_ranges(assert_cast<const ColumnString*>(wkt_column.get())->get_data_at(0));
        const auto& name_with_type = data_type_with_names[0];
        const auto param_type = name_with_type.second->get_type_as_type_descriptor().type;
        auto rows = std::make_shared<roaring::Roaring>();
        for (const auto& range : ranges) {
            auto query = [&](segment_v2::InvertedIndexQueryType type, int64_t value,
                             std::shared_ptr<roaring::Roaring>* out) {
                Field bound(value);
                std::unique_ptr<segment_v2::InvertedIndexQueryParamFactory> param;
                RETURN_IF_ERROR(segment_v2::InvertedIndexQueryParamFactory::create_query_value(
                        param_type, &bound, param));
                *out = std::make_shared<roaring::Roaring>();
                // skip_try: never bypass the index on the per-query hit-count guess.
                return iter->read_from_inverted_index(name_with_type, param->get_value(), type,
                                                      num_rows, *out, /*skip_try=*/true);
            };
            std::shared_ptr<roaring::Roaring> at_least;
            std::shared_ptr<roaring::Roaring> at_most;
            RETURN_IF_ERROR(query(segment_v2::InvertedIndexQueryType::GREATER_EQUAL_QUERY, range.lo,
                                  &at_least));
            RETURN_IF_ERROR(query(segment_v2::InvertedIndexQueryType::LESS_EQUAL_QUERY, range.hi,
                                  &at_most));
            *rows |= *at_least & *at_most;
        }
        // The result is exact, so NULL cells must be reported as NULL (not false): NOT and OR
        // over index results use universe - data - null (audit round 6).
        auto nulls = std::make_shared<roaring::Roaring>();
        // has_null() is Result<bool>: test the value, not whether the Result holds one.
        const bool has_null = DORIS_TRY(iter->has_null());
        if (has_null) {
            segment_v2::InvertedIndexQueryCacheHandle null_bitmap_cache_handle;
            RETURN_IF_ERROR(iter->read_null_bitmap(&null_bitmap_cache_handle));
            *nulls = *null_bitmap_cache_handle.get_bitmap();
            *rows -= *nulls;
        }
        bitmap_result = segment_v2::InvertedIndexResultBitmap(rows, nulls);
        return Status::OK();
    }
};

void register_function_geo_cell(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionS2Cell<false>>();
    factory.register_function<FunctionS2Cell<true>>();
    factory.register_function<FunctionS2Covers>();
}

} // namespace doris::vectorized
