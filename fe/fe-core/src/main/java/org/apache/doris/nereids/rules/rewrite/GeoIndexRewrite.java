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

import org.apache.doris.analysis.Expr;
import org.apache.doris.analysis.FunctionCallExpr;
import org.apache.doris.analysis.SlotRef;
import org.apache.doris.catalog.Column;
import org.apache.doris.catalog.GeneratedColumnInfo;
import org.apache.doris.catalog.OlapTable;
import org.apache.doris.nereids.rules.Rule;
import org.apache.doris.nereids.rules.RuleType;
import org.apache.doris.nereids.trees.expressions.Expression;
import org.apache.doris.nereids.trees.expressions.Slot;
import org.apache.doris.nereids.trees.expressions.SlotReference;
import org.apache.doris.nereids.trees.expressions.functions.scalar.S2Covers;
import org.apache.doris.nereids.trees.expressions.functions.scalar.StContains;
import org.apache.doris.nereids.trees.expressions.functions.scalar.StPoint;
import org.apache.doris.nereids.trees.expressions.functions.scalar.StPolygon;
import org.apache.doris.nereids.trees.expressions.literal.StringLikeLiteral;
import org.apache.doris.nereids.trees.plans.Plan;
import org.apache.doris.nereids.trees.plans.logical.LogicalFilter;
import org.apache.doris.nereids.trees.plans.logical.LogicalOlapScan;
import org.apache.doris.nereids.types.BigIntType;

import com.google.common.collect.ImmutableSet;

import java.util.Optional;
import java.util.Set;

/**
 * Geo index: for a conjunct ST_Contains(ST_Polygon('WKT'), ST_Point(lon, lat)) on a table that has an
 * inverted-indexed column generated AS (s2_cell(lat, lon)), add the conjunct s2_covers(that column, 'WKT').
 * s2_covers is answered from the column's BKD index as a superset of the polygon; the original
 * ST_Contains stays as the exact recheck, so the rewrite narrows the rows read and never changes results.
 */
public class GeoIndexRewrite extends OneRewriteRuleFactory {

    @Override
    public Rule build() {
        return logicalFilter(logicalOlapScan())
                .when(filter -> filter.getConjuncts().stream().anyMatch(StContains.class::isInstance))
                .then(this::addCellPredicates)
                .toRule(RuleType.GEO_INDEX_REWRITE);
    }

    private Plan addCellPredicates(LogicalFilter<LogicalOlapScan> filter) {
        LogicalOlapScan scan = filter.child();
        ImmutableSet.Builder<Expression> added = ImmutableSet.builder();
        for (Expression conjunct : filter.getConjuncts()) {
            if (conjunct instanceof StContains) {
                cellPredicate((StContains) conjunct, scan).ifPresent(added::add);
            }
        }
        Set<Expression> extra = added.build();
        if (extra.isEmpty() || filter.getConjuncts().containsAll(extra)) {
            return null;
        }
        ImmutableSet<Expression> conjuncts = ImmutableSet.<Expression>builder()
                .addAll(filter.getConjuncts()).addAll(extra).build();
        return filter.withConjuncts(conjuncts);
    }

    private Optional<Expression> cellPredicate(StContains contains, LogicalOlapScan scan) {
        if (!(contains.child(0) instanceof StPolygon) || !(contains.child(1) instanceof StPoint)) {
            return Optional.empty();
        }
        Expression wkt = contains.child(0).child(0);
        Expression lon = contains.child(1).child(0);
        Expression lat = contains.child(1).child(1);
        if (!(wkt instanceof StringLikeLiteral) || !(lon instanceof SlotReference)
                || !(lat instanceof SlotReference)) {
            return Optional.empty();
        }
        Optional<Column> lonColumn = ((SlotReference) lon).getOriginalColumn();
        Optional<Column> latColumn = ((SlotReference) lat).getOriginalColumn();
        if (!lonColumn.isPresent() || !latColumn.isPresent()) {
            return Optional.empty();
        }
        OlapTable table = scan.getTable();
        for (Slot slot : scan.getOutput()) {
            Optional<Column> column = ((SlotReference) slot).getOriginalColumn();
            // BIGINT only: s2_covers reads the stored value as the S2 cell id, and a narrower or
            // string column may already have lost it (audit 2026-11-19 C.3b #2).
            if (column.isPresent() && slot.getDataType().equals(BigIntType.INSTANCE)
                    && isCellOf(column.get().getGeneratedColumnInfo(), latColumn.get(), lonColumn.get())
                    && table.getInvertedIndex(column.get(), null) != null) {
                return Optional.of(new S2Covers(slot, wkt));
            }
        }
        return Optional.empty();
    }

    // True when the generated column is exactly s2_cell(lat, lon) over these two columns: a
    // structural match on the bound expression, not on its SQL text (audit C.3b #1).  The 2-argument
    // form is level 16, the level s2_covers assumes.
    private static boolean isCellOf(GeneratedColumnInfo info, Column lat, Column lon) {
        if (info == null || !(info.getExpr() instanceof FunctionCallExpr)) {
            return false;
        }
        FunctionCallExpr call = (FunctionCallExpr) info.getExpr();
        if (!"s2_cell".equalsIgnoreCase(call.getFnName().getFunction()) || call.getChildren().size() != 2) {
            return false;
        }
        return refersTo(call.getChild(0), lat) && refersTo(call.getChild(1), lon);
    }

    private static boolean refersTo(Expr expr, Column column) {
        return expr instanceof SlotRef && ((SlotRef) expr).getColumnName().equalsIgnoreCase(column.getName());
    }
}
