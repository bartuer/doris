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

import java.util.regex.Matcher
import java.util.regex.Pattern
import org.apache.doris.regression.action.ProfileAction

// Geo index (plan 01): an inverted-indexed BIGINT column generated as s2_cell(lat, lon) makes
// ST_Contains(ST_Polygon(<const wkt>), ST_Point(lon, lat)) read only the polygon's S2 cover.
// Correctness bar (G3): for every polygon the indexed table returns exactly the rows the same
// query returns on an identical table without the cell column.
suite("test_geo_index") {
    sql "DROP TABLE IF EXISTS geo_index_pts"
    sql "DROP TABLE IF EXISTS geo_plain_pts"
    sql """
        CREATE TABLE geo_index_pts (
            id BIGINT NOT NULL,
            lon DOUBLE NULL,
            lat DOUBLE NULL,
            geo_cell BIGINT AS (s2_cell(lat, lon)),
            INDEX idx_geo (geo_cell) USING INVERTED
        ) DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 4
        PROPERTIES ("replication_num" = "1")
    """
    sql """
        CREATE TABLE geo_plain_pts (
            id BIGINT NOT NULL,
            lon DOUBLE NULL,
            lat DOUBLE NULL
        ) DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 4
        PROPERTIES ("replication_num" = "1")
    """

    // 20,000 deterministic points: dense around Beijing, sparse world-wide, a band around the
    // antimeridian, a cap near the north pole.
    def points = """
        SELECT number AS id,
          CASE number % 4
            WHEN 0 THEN 116.0 + (murmur_hash3_64(number) % 1000000 + 1000000) % 1000000 / 1000000.0 * 0.8
            WHEN 1 THEN -180 + (murmur_hash3_64(number * 7 + 1) % 1000000 + 1000000) % 1000000 / 1000000.0 * 360
            WHEN 2 THEN (CASE WHEN number % 8 = 2 THEN 178 ELSE -180 END)
                        + (murmur_hash3_64(number * 3) % 1000000 + 1000000) % 1000000 / 1000000.0 * 2
            ELSE -180 + (murmur_hash3_64(number * 5) % 1000000 + 1000000) % 1000000 / 1000000.0 * 360 END,
          CASE number % 4
            WHEN 0 THEN 39.6 + (murmur_hash3_64(number + 12345) % 1000000 + 1000000) % 1000000 / 1000000.0 * 0.6
            WHEN 1 THEN -85 + (murmur_hash3_64(number * 7 + 2) % 1000000 + 1000000) % 1000000 / 1000000.0 * 170
            WHEN 2 THEN -18 + (murmur_hash3_64(number * 3 + 1) % 1000000 + 1000000) % 1000000 / 1000000.0 * 4
            ELSE 80 + (murmur_hash3_64(number * 5 + 1) % 1000000 + 1000000) % 1000000 / 1000000.0 * 10 END
        FROM numbers("number" = "20000")
    """
    sql "INSERT INTO geo_index_pts (id, lon, lat) ${points}"
    sql "INSERT INTO geo_plain_pts (id, lon, lat) ${points}"
    // Exact polygon vertices and edge points, NULL coordinates and out-of-range values.
    def specials = """
        VALUES (900001, 116.20, 39.75), (900002, 116.55, 40.05), (900003, 116.375, 40.05),
               (900004, 116.375, 39.75), (900005, 180.0, -16.0), (900006, -180.0, -16.0),
               (900007, 0.0, 90.0), (900008, NULL, 39.9), (900009, 116.4, NULL),
               (900010, 200.0, 39.9), (900011, 116.4, 95.0)
    """
    sql "INSERT INTO geo_index_pts (id, lon, lat) ${specials}"
    sql "INSERT INTO geo_plain_pts (id, lon, lat) ${specials}"

    def polygons = [
        city: "POLYGON((116.20 39.75, 116.55 39.75, 116.60 39.90, 116.55 40.05, 116.20 40.05, 116.15 39.90, 116.20 39.75))",
        thin: "POLYGON((116.00 39.60, 116.80 40.20, 116.79 40.21, 115.99 39.61, 116.00 39.60))",
        concave: "POLYGON((116.00 39.60, 116.80 39.60, 116.80 40.20, 116.60 40.20, 116.60 39.80, 116.20 39.80, 116.20 40.20, 116.00 40.20, 116.00 39.60))",
        hole: "POLYGON((116.20 39.75, 116.55 39.75, 116.55 40.05, 116.20 40.05, 116.20 39.75), (116.30 39.85, 116.45 39.85, 116.45 39.95, 116.30 39.95, 116.30 39.85))",
        antimeridian: "POLYGON((179 -17, -179 -17, -179 -15, 179 -15, 179 -17))",
        pole_cap: "POLYGON((-180 85, -90 85, 0 85, 90 85, 180 85, -180 85))",
        empty_area: "POLYGON((10 10, 10.001 10, 10.001 10.001, 10 10.001, 10 10))"
    ]

    // The planner adds the cell predicate to a plain ST_Contains on the indexed table.
    explain {
        sql "SELECT id FROM geo_index_pts WHERE ST_Contains(ST_Polygon('${polygons.city}'), ST_Point(lon, lat))"
        contains "s2_covers(geo_cell"
    }
    explain {
        sql "SELECT id FROM geo_plain_pts WHERE ST_Contains(ST_Polygon('${polygons.city}'), ST_Point(lon, lat))"
        notContains "s2_covers"
    }

    // G3, per polygon: rows only in the indexed result, rows only in the plain result (both must be
    // 0), and the hit count.  One generated .out line per polygon.
    def diff = polygons.collect { name, wkt -> """
        SELECT '${name}' AS polygon,
          (SELECT count(*) FROM (
              SELECT id FROM geo_index_pts WHERE ST_Contains(ST_Polygon('${wkt}'), ST_Point(lon, lat))
              EXCEPT
              SELECT id FROM geo_plain_pts WHERE ST_Contains(ST_Polygon('${wkt}'), ST_Point(lon, lat))) a) AS only_indexed,
          (SELECT count(*) FROM (
              SELECT id FROM geo_plain_pts WHERE ST_Contains(ST_Polygon('${wkt}'), ST_Point(lon, lat))
              EXCEPT
              SELECT id FROM geo_index_pts WHERE ST_Contains(ST_Polygon('${wkt}'), ST_Point(lon, lat))) b) AS only_plain,
          (SELECT count(*) FROM geo_index_pts WHERE ST_Contains(ST_Polygon('${wkt}'), ST_Point(lon, lat))) AS hits
    """ }.join(" UNION ALL ")
    order_qt_indexed_equals_plain diff

    // The differential check above also passes on the row path, so prove the BKD index path runs:
    // the rewritten city query must filter rows through the inverted index (audit Z.2 #1).
    sql "set enable_profile = true"
    sql "set profile_level = 2"
    sql "set enable_sql_cache = false"
    def profileAction = new ProfileAction(context)
    def token = "geo_index_profile_" + UUID.randomUUID().toString()
    sql """SELECT /* ${token} */ count(*) FROM geo_index_pts
        WHERE ST_Contains(ST_Polygon('${polygons.city}'), ST_Point(lon, lat))"""
    // 3.1's ProfileAction has no getProfileBySql: find the profile id by the token.
    def profileId = null
    for (int i = 0; i < 20 && profileId == null; i++) {
        profileId = profileAction.getProfileList().find { it["Sql Statement"].contains(token) }?.get("Profile ID")
        if (profileId == null) { Thread.sleep(500) }
    }
    String profileString = profileAction.getProfile(profileId).toString()
    Matcher matcher = Pattern.compile("RowsInvertedIndexFiltered:\\s*([0-9,]+)").matcher(profileString)
    long indexFiltered = 0
    while (matcher.find()) {
        indexFiltered += Long.parseLong(matcher.group(1).replace(",", ""))
    }
    assertTrue(indexFiltered > 0, "geo index not used, profile: ${profileString}")
    sql "set enable_profile = false"

    // NULL and out-of-range coordinates: s2_cell is NULL, the row is never a hit.
    order_qt_specials """SELECT id, geo_cell IS NULL FROM geo_index_pts WHERE id >= 900001 ORDER BY id"""
    // NULL cells stay NULL under NOT / OR on the index path (audit round 6): compare with the row path.
    def negated = """SELECT count(*) FROM geo_index_pts
        WHERE NOT s2_covers(geo_cell, '${polygons.city}') OR id = 900008"""
    def viaIndex = sql negated
    sql "set enable_inverted_index_query = false"
    def viaRows = sql negated
    sql "set enable_inverted_index_query = true"
    assertEquals(viaRows, viaIndex)
    // s2_covers on its own (the explicit form) agrees with the rewritten query.
    order_qt_explicit """
        SELECT count(*) FROM geo_index_pts
        WHERE s2_covers(geo_cell, '${polygons.city}') AND ST_Contains(ST_Polygon('${polygons.city}'), ST_Point(lon, lat))
    """
}
