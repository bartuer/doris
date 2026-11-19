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

#include <gtest/gtest.h>
#include <s2/s2cell.h>
#include <s2/s2cell_id.h>
#include <s2/s2latlng.h>
#include <s2/s2loop.h>
#include <s2/s2point.h>
#include <s2/s2polygon.h>

#include <algorithm>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "geo_index/cell.h"

#ifndef GEO_TEST_DATA_DIR
#error "GEO_TEST_DATA_DIR must point at test/data"
#endif

namespace geo_index {
namespace {

struct Point {
    int64_t id;
    double lon;
    double lat;
};

std::vector<Point> load_points() {
    std::ifstream in(std::string(GEO_TEST_DATA_DIR) + "/points.csv");
    EXPECT_TRUE(in.good());
    std::string line;
    std::getline(in, line); // header
    std::vector<Point> pts;
    char comma;
    Point p;
    while (in >> p.id >> comma >> p.lon >> comma >> p.lat) {
        pts.push_back(p);
    }
    return pts;
}

// "lon lat,lon lat,..." -> S2Loop (vertices in lat/lon degrees; S2 normalizes orientation).
std::unique_ptr<S2Loop> parse_ring(const std::string& ring) {
    std::vector<S2Point> vertices;
    std::stringstream ss(ring);
    std::string pair;
    while (std::getline(ss, pair, ',')) {
        double lon;
        double lat;
        std::stringstream(pair) >> lon >> lat;
        vertices.push_back(S2LatLng::FromDegrees(lat, lon).ToPoint());
    }
    auto loop = std::make_unique<S2Loop>(vertices);
    loop->Normalize(); // at most a hemisphere, like Doris' GeoPolygon
    return loop;
}

std::vector<std::pair<std::string, std::unique_ptr<S2Polygon>>> load_polygons() {
    std::ifstream in(std::string(GEO_TEST_DATA_DIR) + "/polygons.txt");
    EXPECT_TRUE(in.good());
    std::vector<std::pair<std::string, std::unique_ptr<S2Polygon>>> out;
    std::string line;
    while (std::getline(in, line)) {
        std::stringstream ss(line);
        std::string name;
        std::getline(ss, name, '|');
        std::vector<std::unique_ptr<S2Loop>> loops;
        std::string ring;
        while (std::getline(ss, ring, '|')) {
            loops.push_back(parse_ring(ring));
        }
        auto polygon = std::make_unique<S2Polygon>();
        polygon->InitNested(std::move(loops));
        EXPECT_TRUE(polygon->IsValid()) << name;
        out.emplace_back(name, std::move(polygon));
    }
    return out;
}

bool in_ranges(const std::vector<CellRange>& ranges, int64_t cell) {
    auto it = std::upper_bound(ranges.begin(), ranges.end(), cell,
                               [](int64_t c, const CellRange& r) { return c < r.lo; });
    return it != ranges.begin() && cell <= std::prev(it)->hi;
}

} // namespace

TEST(GeoIndexCover, NoFalseNegativesOnFixtures) {
    auto points = load_points();
    auto polygons = load_polygons();
    ASSERT_GT(points.size(), 9000u);
    ASSERT_EQ(6u, polygons.size());
    for (int level : {10, 16, 20}) {
        for (int max_cells : {8, 64}) {
            for (const auto& [name, polygon] : polygons) {
                auto ranges = cover(*polygon, level, max_cells);
                ASSERT_FALSE(ranges.empty()) << name;
                // Sorted, disjoint, non-adjacent.
                for (size_t i = 0; i < ranges.size(); ++i) {
                    ASSERT_LE(ranges[i].lo, ranges[i].hi) << name;
                    if (i > 0) {
                        ASSERT_LT(ranges[i - 1].hi + 1, ranges[i].lo) << name;
                    }
                }
                int hits = 0;
                int candidates = 0;
                for (const Point& p : points) {
                    S2Point s2p = S2LatLng::FromDegrees(p.lat, p.lon).Normalized().ToPoint();
                    bool inside = polygon->Contains(s2p);
                    bool candidate = in_ranges(ranges, cell_of(p.lat, p.lon, level));
                    hits += inside;
                    candidates += candidate;
                    ASSERT_TRUE(candidate || !inside)
                            << name << " level " << level << " max_cells " << max_cells
                            << ": false negative id " << p.id << " (" << p.lon << " " << p.lat << ")";
                }
                EXPECT_GT(hits, 0) << name;
                EXPECT_GE(candidates, hits) << name;
            }
        }
    }
}

TEST(GeoIndexCover, FinerLevelIsTighter) {
    auto points = load_points();
    auto polygons = load_polygons();
    const S2Polygon& city = *polygons[0].second;
    ASSERT_EQ("city", polygons[0].first);
    auto count = [&](int level) {
        auto ranges = cover(city, level, 64);
        int n = 0;
        for (const Point& p : points) {
            n += in_ranges(ranges, cell_of(p.lat, p.lon, level));
        }
        return n;
    };
    EXPECT_LE(count(16), count(8));
}

// Audit 2026-11-19 #1: a point on a covering cell's edge is assigned to the
// neighbouring cell.  Face-1 square as a polygon; (0, 45) lies on its edge.
TEST(GeoIndexCover, PointOnCoveringCellEdgeIsCovered) {
    S2Polygon face1(S2Cell(S2CellId::FromFace(1)));
    std::vector<std::pair<double, double>> edge_points = {{0, 45}, {0, 135}, {45, 90}, {-45, 90}};
    for (int level : {0, 1, 10, 30}) {
        auto ranges = cover(face1, level, 8);
        for (auto [lat, lon] : edge_points) {
            S2Point p = S2LatLng::FromDegrees(lat, lon).ToPoint();
            if (!face1.Contains(p)) {
                continue; // S2Polygon is half-open: this edge belongs to the other side
            }
            EXPECT_TRUE(in_ranges(ranges, cell_of(lat, lon, level)))
                    << "level " << level << " (" << lat << " " << lon << ")";
        }
        // The fixture case the auditor reproduced, regardless of Contains().
        EXPECT_TRUE(in_ranges(ranges, cell_of(0, 45, level))) << "level " << level;
    }
}

TEST(GeoIndexCoverDeathTest, BadArgumentsAbort) {
    S2Polygon empty;
    EXPECT_DEATH(cover(empty, 31, 8), "GEO_CHECK");
    EXPECT_DEATH(cover(empty, 10, 0), "GEO_CHECK");
}

TEST(GeoIndexCover, EmptyPolygonGivesNoRanges) {
    S2Polygon empty;
    EXPECT_TRUE(cover(empty, 16, 8).empty());
}

} // namespace geo_index
