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
// The arithmetic lives in contrib/geo_index_core; this file is the Doris adapter.

#include <cmath>

#include "core/block/block.h"
#include "core/column/column_execute_util.h"
#include "core/column/column_nullable.h"
#include "core/data_type/data_type_nullable.h"
#include "core/data_type/data_type_number.h"
#include "exprs/function/simple_function_factory.h"
#include "geo_index/cell.h"
#include "geo_index/cover.h"

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

void register_function_geo_cell(SimpleFunctionFactory& factory) {
    factory.register_function<FunctionS2Cell<false>>();
    factory.register_function<FunctionS2Cell<true>>();
}

} // namespace doris
