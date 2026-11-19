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

// Property test for cover(): no false negatives on points that sit exactly on
// S2 cell vertices and edges (where S2CellId(point) picks one of several cells),
// plus random points, over random polygons.  Fixed seed: deterministic.

#include <gtest/gtest.h>
#include <s2/s2cell.h>
#include <s2/s2cell_union.h>
#include <s2/s2latlng.h>
#include <s2/s2loop.h>
#include <s2/s2polygon.h>
#include <s2/s2region_coverer.h>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <memory>
#include <random>
#include <vector>

#include "geo_index/cell.h"
#include "geo_index/cover.h"

namespace geo_index {
namespace {

bool in_ranges(const std::vector<CellRange>& ranges, int64_t cell) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), cell,
                               [](int64_t c, const CellRange& r) { return c < r.lo; });
    return it != ranges.begin() && cell <= std::prev(it)->hi;
}

// A star-shaped loop around (lat, lon): convex or concave by radius jitter.
std::unique_ptr<S2Loop> random_loop(std::mt19937_64& rng, double lat, double lon, double radius) {
    std::uniform_real_distribution<double> jitter(0.4, 1.0);
    int n = 4 + static_cast<int>(rng() % 9);
    std::vector<S2Point> vertices;
    for (int i = 0; i < n; ++i) {
        double a = 2 * M_PI * i / n;
        double r = radius * jitter(rng);
        vertices.push_back(S2LatLng::FromDegrees(lat + r * std::sin(a), lon + r * std::cos(a))
                                   .Normalized()
                                   .ToPoint());
    }
    auto loop = std::make_unique<S2Loop>(vertices);
    loop->Normalize();
    return loop;
}

std::unique_ptr<S2Polygon> random_polygon(std::mt19937_64& rng, int i) {
    std::uniform_real_distribution<double> u(-1, 1);
    double lat = u(rng) * 70;
    double lon = u(rng) * 180;
    if (i % 10 == 0) {
        lon = 179.9; // straddles the antimeridian
    }
    if (i % 10 == 1) {
        lat = 89.0; // near the north pole
    }
    double radius = 0.01 * std::pow(300.0, (u(rng) + 1) / 2); // 0.01 .. 3 degrees
    std::vector<std::unique_ptr<S2Loop>> loops;
    loops.push_back(random_loop(rng, lat, lon, radius));
    if (i % 4 == 0) {
        loops.push_back(random_loop(rng, lat, lon, radius * 0.3)); // hole
    }
    auto polygon = std::make_unique<S2Polygon>();
    polygon->InitNested(std::move(loops));
    return polygon;
}

} // namespace

TEST(GeoIndexCoverProperty, NoFalseNegativesOnCellBoundaries) {
    std::mt19937_64 rng(42);
    std::uniform_real_distribution<double> unit(0, 1);
    long checked = 0;
    for (int i = 0; i < 300; ++i) {
        std::unique_ptr<S2Polygon> owned = random_polygon(rng, i);
        const S2Polygon& polygon = *owned;
        ASSERT_TRUE(polygon.IsValid()) << "polygon " << i;
        S2LatLngRect bound = polygon.GetRectBound();
        for (int level : {8, 12, 16, 20}) {
            for (int max_cells : {8, 64}) {
                auto ranges = cover(polygon, level, max_cells);
                std::vector<S2Point> probes;
                // Vertices and edge midpoints of the plain covering's cells (the
                // boundaries cover() must see past), at most 64 cells per case.
                S2RegionCoverer::Options options;
                options.set_max_level(level);
                options.set_max_cells(max_cells);
                S2CellUnion plain = S2RegionCoverer(options).GetCovering(polygon);
                for (const S2CellId& id : plain) {
                    S2Cell cell(id);
                    for (int k = 0; k < 4; ++k) {
                        S2Point a = cell.GetVertex(k);
                        S2Point b = cell.GetVertex((k + 1) % 4);
                        probes.push_back(a);
                        probes.push_back((a + b).Normalize());
                    }
                }
                // The polygon's own boundary: vertices and edge points.  These are
                // asserted WITHOUT the Contains() filter in PolygonBoundaryIsCovered.
                // Random points in the bounding rectangle.
                for (int k = 0; k < 64; ++k) {
                    double lat = bound.lat_lo().degrees() +
                                 unit(rng) * (bound.lat_hi() - bound.lat_lo()).degrees();
                    double lon = bound.lng_lo().degrees() + unit(rng) * bound.lng().GetLength() *
                                                                    180 / M_PI;
                    probes.push_back(S2LatLng::FromDegrees(lat, lon).Normalized().ToPoint());
                }
                for (const S2Point& probe : probes) {
                    // Oracle on the exact point cell_of() sees (audit round 2 #1): the
                    // probe goes through degrees, as a Doris row does.
                    S2LatLng ll(probe);
                    double lat = ll.lat().degrees();
                    double lon = ll.lng().degrees();
                    if (!polygon.Contains(S2LatLng::FromDegrees(lat, lon).Normalized().ToPoint())) {
                        continue;
                    }
                    ++checked;
                    ASSERT_TRUE(in_ranges(ranges, cell_of(lat, lon, level)))
                            << "polygon " << i << " level " << level << " max_cells " << max_cells
                            << " point " << ll;
                }
            }
        }
    }
    RecordProperty("checked", static_cast<int>(checked));
    std::printf("property: %ld inside points checked, 0 false negatives\n", checked);
    EXPECT_GT(checked, 10000);
}

// Audit round 2 #1: S2Polygon::Contains is semi-open, so the test above skips
// some boundary points.  The cover contract includes the boundary, so assert it
// on every polygon vertex and on points along every edge, with no Contains() filter.
TEST(GeoIndexCoverProperty, PolygonBoundaryIsCovered) {
    std::mt19937_64 rng(7);
    long checked = 0;
    for (int i = 0; i < 300; ++i) {
        std::unique_ptr<S2Polygon> polygon = random_polygon(rng, i);
        for (int level : {8, 12, 16, 20}) {
            auto ranges = cover(*polygon, level, 8);
            for (int l = 0; l < polygon->num_loops(); ++l) {
                const S2Loop& loop = *polygon->loop(l);
                for (int v = 0; v < loop.num_vertices(); ++v) {
                    S2Point a = loop.vertex(v);
                    S2Point b = loop.vertex(v + 1);
                    for (double t : {0.0, 0.25, 0.5, 0.75}) {
                        S2LatLng ll(((1 - t) * a + t * b).Normalize());
                        ++checked;
                        ASSERT_TRUE(in_ranges(ranges, cell_of(ll.lat().degrees(), ll.lng().degrees(), level)))
                                << "polygon " << i << " level " << level << " boundary point " << ll;
                    }
                }
            }
        }
    }
    std::printf("boundary: %ld polygon boundary points checked, 0 false negatives\n", checked);
    EXPECT_GT(checked, 10000);
}

} // namespace geo_index
