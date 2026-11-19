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

// Plan 02 P.1: Google Benchmark for the core.
//   BM_CellOf                 ns per point (the per-row cost when a row is loaded)
//   BM_Cover/<poly>/<level>/<max_cells>  time per query + ranges
// Selectivity (candidate rows / true hits) is a separate pass over the
// checked-in fixture points: --geo_selectivity prints a TSV table and exits.

#include <benchmark/benchmark.h>
#include <s2/s2latlng.h>
#include <s2/s2loop.h>
#include <s2/s2polygon.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "geo_index/cell.h"
#include "geo_index/cover.h"

namespace {

using geo_index::CellRange;

struct Point {
    double lon;
    double lat;
};

std::vector<Point> load_points() {
    std::ifstream in(std::string(GEO_TEST_DATA_DIR) + "/points.csv");
    std::string line;
    std::getline(in, line);
    std::vector<Point> pts;
    long id;
    char comma;
    Point p;
    while (in >> id >> comma >> p.lon >> comma >> p.lat) {
        pts.push_back(p);
    }
    return pts;
}

std::vector<std::pair<std::string, std::unique_ptr<S2Polygon>>> load_polygons() {
    std::ifstream in(std::string(GEO_TEST_DATA_DIR) + "/polygons.txt");
    std::vector<std::pair<std::string, std::unique_ptr<S2Polygon>>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string name;
        std::getline(ss, name, '|');
        std::vector<std::unique_ptr<S2Loop>> loops;
        std::string ring;
        while (std::getline(ss, ring, '|')) {
            std::vector<S2Point> v;
            std::stringstream rs(ring);
            std::string pair;
            while (std::getline(rs, pair, ',')) {
                double lon;
                double lat;
                std::stringstream(pair) >> lon >> lat;
                v.push_back(S2LatLng::FromDegrees(lat, lon).ToPoint());
            }
            loops.push_back(std::make_unique<S2Loop>(v));
            loops.back()->Normalize();
        }
        auto polygon = std::make_unique<S2Polygon>();
        polygon->InitNested(std::move(loops));
        out.emplace_back(name, std::move(polygon));
    }
    return out;
}

const auto& polygons() {
    static const auto p = load_polygons();
    return p;
}

bool in_ranges(const std::vector<CellRange>& ranges, int64_t cell) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), cell,
                               [](int64_t c, const CellRange& r) { return c < r.lo; });
    return it != ranges.begin() && cell <= std::prev(it)->hi;
}

void BM_CellOf(benchmark::State& state) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> lat(-90, 90);
    std::uniform_real_distribution<double> lon(-180, 180);
    std::vector<Point> pts(1 << 16);
    for (auto& p : pts) {
        p = {lon(rng), lat(rng)};
    }
    int level = static_cast<int>(state.range(0));
    size_t i = 0;
    for (auto _ : state) {
        const Point& p = pts[i++ & (pts.size() - 1)];
        benchmark::DoNotOptimize(geo_index::cell_of(p.lat, p.lon, level));
    }
    state.SetItemsProcessed(state.iterations());
}
BENCHMARK(BM_CellOf)->Arg(12)->Arg(16)->Arg(20)->Arg(30);

void BM_Cover(benchmark::State& state) {
    const auto& [name, polygon] = polygons()[state.range(0)];
    int level = static_cast<int>(state.range(1));
    int max_cells = static_cast<int>(state.range(2));
    size_t ranges = 0;
    for (auto _ : state) {
        auto r = geo_index::cover(*polygon, level, max_cells);
        ranges = r.size();
        benchmark::DoNotOptimize(r.data());
    }
    state.SetLabel(name);
    state.counters["ranges"] = static_cast<double>(ranges);
}
BENCHMARK(BM_Cover)
        ->ArgsProduct({{0, 1, 2, 3, 4, 5}, {12, 16, 20}, {8, 64, 256, 1024}})
        ->Unit(benchmark::kMicrosecond);

// candidate rows / true hits over the fixture points, per polygon and setting.
int print_selectivity() {
    auto points = load_points();
    std::printf("polygon\tlevel\tmax_cells\tranges\thits\tcandidates\tratio\n");
    for (const auto& [name, polygon] : polygons()) {
        for (int level : {12, 16, 20}) {
            for (int max_cells : {8, 64, 256, 1024}) {
                {
                    auto ranges = geo_index::cover(*polygon, level, max_cells);
                    long hits = 0;
                    long candidates = 0;
                    for (const Point& p : points) {
                        hits += polygon->Contains(S2LatLng::FromDegrees(p.lat, p.lon).Normalized().ToPoint());
                        candidates += in_ranges(ranges, geo_index::cell_of(p.lat, p.lon, level));
                    }
                    std::printf("%s\t%d\t%d\t%zu\t%ld\t%ld\t%.2f\n", name.c_str(), level, max_cells,
                                ranges.size(), hits, candidates,
                                hits == 0 ? 0.0 : static_cast<double>(candidates) / hits);
                }
            }
        }
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--geo_selectivity") == 0) {
            return print_selectivity();
        }
    }
    benchmark::Initialize(&argc, argv);
    benchmark::RunSpecifiedBenchmarks();
    benchmark::Shutdown();
    return 0;
}
