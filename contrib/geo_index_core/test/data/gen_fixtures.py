#!/usr/bin/env python3
# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#   http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing,
# software distributed under the License is distributed on an
# "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
# KIND, either express or implied.  See the License for the
# specific language governing permissions and limitations
# under the License.
"""Regenerate the geo index test fixtures (deterministic, seed 42).

polygons.txt: one polygon per line: name|shell|hole|hole...
              each ring = "lon lat,lon lat,..." (not closed; GeoJSON order lon, lat)
points.csv:   id,lon,lat
"""
import math
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))

POLYGONS = {
    # The three plan-01 baseline polygons (Beijing).
    "city": [[(116.20, 39.75), (116.55, 39.75), (116.60, 39.90), (116.55, 40.05), (116.20, 40.05), (116.15, 39.90)]],
    "thin": [[(115.50, 39.50), (117.50, 40.90), (117.49, 40.91), (115.49, 39.51)]],
    "concave": [[(116.00, 39.60), (117.00, 39.60), (117.00, 40.40), (116.80, 40.40), (116.80, 39.80),
                 (116.20, 39.80), (116.20, 40.40), (116.00, 40.40)]],
    "hole": [[(10.0, 10.0), (12.0, 10.0), (12.0, 12.0), (10.0, 12.0)],
             [(10.5, 10.5), (11.5, 10.5), (11.5, 11.5), (10.5, 11.5)]],
    "antimeridian": [[(179.0, -17.0), (-179.0, -17.0), (-179.0, -15.0), (179.0, -15.0)]],
    "pole_cap": [[(float(lon), 80.0) for lon in range(-180, 180, 30)]],
}

def ring_points(ring):
    """Vertices and edge midpoints (in lat/lon, i.e. near the geodesic edge)."""
    out = list(ring)
    for (x0, y0), (x1, y1) in zip(ring, ring[1:] + ring[:1]):
        dx = x1 - x0
        if dx > 180:
            dx -= 360
        if dx < -180:
            dx += 360
        x = x0 + dx / 2
        x = x - 360 if x > 180 else x
        out.append((x, (y0 + y1) / 2))
    return out

def main():
    rnd = random.Random(42)
    pts = []
    # Uniform on the sphere.
    for _ in range(4000):
        pts.append((rnd.uniform(-180, 180), math.degrees(math.asin(rnd.uniform(-1, 1)))))
    # Dense around each polygon's bounding box (+ margin).
    for rings in POLYGONS.values():
        xs = [x for x, _ in rings[0]]
        ys = [y for _, y in rings[0]]
        if max(xs) - min(xs) > 180:  # crosses the antimeridian
            xs = [x + 360 if x < 0 else x for x in xs]
        x0, x1, y0, y1 = min(xs) - 0.5, max(xs) + 0.5, min(ys) - 0.5, min(max(ys) + 0.5, 90)
        for _ in range(900):
            x = rnd.uniform(x0, x1)
            pts.append((x - 360 if x > 180 else x, rnd.uniform(y0, y1)))
        for ring in rings:
            pts.extend(ring_points(ring))
    # Edges of the lat/lon domain.
    pts += [(180.0, 0.0), (-180.0, 0.0), (0.0, 90.0), (0.0, -90.0), (180.0, -16.0), (-180.0, -16.0)]

    with open(os.path.join(HERE, "points.csv"), "w") as f:
        f.write("id,lon,lat\n")
        for i, (x, y) in enumerate(pts):
            f.write(f"{i},{x:.9f},{y:.9f}\n")
    with open(os.path.join(HERE, "polygons.txt"), "w") as f:
        for name, rings in POLYGONS.items():
            f.write(name + "|" + "|".join(",".join(f"{x} {y}" for x, y in r) for r in rings) + "\n")

if __name__ == "__main__":
    main()
