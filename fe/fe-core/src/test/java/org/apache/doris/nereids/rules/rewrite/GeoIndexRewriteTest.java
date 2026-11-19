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

package org.apache.doris.nereids.rules.rewrite;

import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.SlotReference;
import org.apache.doris.nereids.trees.expressions.functions.scalar.S2Covers;
import org.apache.doris.nereids.trees.expressions.functions.scalar.StContains;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.util.PlanChecker;
import org.apache.doris.utframe.TestWithFeService;

import org.junit.jupiter.api.Assertions;
import org.junit.jupiter.api.Test;

import java.util.List;
import java.util.stream.Collectors;

class GeoIndexRewriteTest extends TestWithFeService {
    private static final String CITY = "POLYGON((116.20 39.75, 116.55 39.75, 116.60 39.90, 116.55 40.05,"
            + " 116.20 40.05, 116.15 39.90, 116.20 39.75))";

    @Override
    protected void runBeforeAll() throws Exception {
        createDatabase("geo_rewrite");
        connectContext.setDatabase("geo_rewrite");
        connectContext.getSessionVariable().setDisableNereidsRules("PRUNE_EMPTY_PARTITION");
        String columns = "id BIGINT NOT NULL, lon DOUBLE NOT NULL, lat DOUBLE NOT NULL, ";
        String tail = " DUPLICATE KEY(id) DISTRIBUTED BY HASH(id) BUCKETS 1 PROPERTIES ('replication_num' = '1')";
        createTable("CREATE TABLE geo_idx (" + columns
                + "geo_cell BIGINT AS (s2_cell(lat, lon)), INDEX idx_cell (geo_cell) USING INVERTED)" + tail);
        createTable("CREATE TABLE geo_no_index (" + columns + "geo_cell BIGINT AS (s2_cell(lat, lon)))" + tail);
        createTable("CREATE TABLE geo_other_order (" + columns
                + "geo_cell BIGINT AS (s2_cell(lon, lat)), INDEX idx_cell (geo_cell) USING INVERTED)" + tail);
        createTable("CREATE TABLE geo_plain (" + columns.substring(0, columns.length() - 2) + ")" + tail);
        // Audit C.3b #1: a column whose name differs from lat only by a space must not match.
        createTable("CREATE TABLE geo_spaced (" + columns + "`la t` DOUBLE NOT NULL, "
                + "geo_cell BIGINT AS (s2_cell(`la t`, lon)), INDEX idx_cell (geo_cell) USING INVERTED)" + tail);
        // Audit C.3b #2: the cell must be stored as BIGINT.
        createTable("CREATE TABLE geo_varchar (" + columns
                + "geo_cell VARCHAR(32) AS (s2_cell(lat, lon)), INDEX idx_cell (geo_cell) USING INVERTED)" + tail);
    }

    private List<Expression> covers(String sql) {
        Plan plan = PlanChecker.from(connectContext).analyze(sql).rewrite().getCascadesContext().getRewritePlan();
        return collect(plan, S2Covers.class);
    }

    // Expressions of the given class anywhere in the plan (collectToList on a plan walks plan nodes).
    private static List<Expression> collect(Plan plan, Class<? extends Expression> type) {
        List<Plan> nodes = plan.collectToList(node -> true);
        return nodes.stream().flatMap(node -> node.getExpressions().stream())
                .flatMap(expression -> expression.<Expression>collectToList(type::isInstance).stream())
                .collect(Collectors.toList());
    }

    private String query(String table) {
        return "SELECT id FROM " + table + " WHERE st_contains(st_polygon('" + CITY + "'), st_point(lon, lat))";
    }

    @Test
    void addsCellPredicateNextToStContains() {
        Plan plan = PlanChecker.from(connectContext).analyze(query("geo_idx")).rewrite()
                .getCascadesContext().getRewritePlan();
        List<Expression> covers = collect(plan, S2Covers.class);
        Assertions.assertEquals(1, covers.size(), plan.treeString());
        Expression cell = covers.get(0).child(0);
        Assertions.assertTrue(cell instanceof SlotReference, plan.treeString());
        Assertions.assertEquals("geo_cell", ((SlotReference) cell).getName());
        // The exact predicate stays: the cell predicate only narrows rows.
        Assertions.assertEquals(1, collect(plan, StContains.class).size(), plan.treeString());
    }

    @Test
    void skipsWithoutIndexOrMatchingCellColumn() {
        Assertions.assertTrue(covers(query("geo_no_index")).isEmpty());
        // s2_cell(lon, lat) is a different cell; using it would drop rows.
        Assertions.assertTrue(covers(query("geo_other_order")).isEmpty());
        Assertions.assertTrue(covers(query("geo_plain")).isEmpty());
        Assertions.assertTrue(covers(query("geo_spaced")).isEmpty());
        Assertions.assertTrue(covers(query("geo_varchar")).isEmpty());
    }

    @Test
    void skipsNonConstantPolygonAndOtherShapes() {
        Assertions.assertTrue(covers("SELECT id FROM geo_idx WHERE st_contains(st_polygon(concat('POLYGON((', "
                + "cast(lon as string), ' 0, 1 1, 0 1, 0 0))')), st_point(lon, lat))").isEmpty());
        Assertions.assertTrue(covers("SELECT id FROM geo_idx WHERE st_contains(st_circle(116.4, 39.9, 1000), "
                + "st_point(lon, lat))").isEmpty());
        Assertions.assertTrue(covers("SELECT id FROM geo_idx WHERE st_contains(st_polygon('" + CITY + "'), "
                + "st_point(lat, lon))").isEmpty());
    }
}
