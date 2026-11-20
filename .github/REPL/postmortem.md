# doris geo index -- Postmortem

Scope: plans 01 (geo index aligned with the ES interface) and 02 (geo_index_core library), on fork
`bartuer/doris`, branches `geo-index` (master), `geo-index-4.1`, `geo-index-3.1`.
Window: 2026-11-19 12:00Z -> 2026-11-20 02:51Z (~15 h wall clock).  All numbers below are measured;
sources are in `fix.archive/`.  Chinese brief: `postmortem.zh_CN.md`.

## 0. The one-paragraph version

Doris has no spatial index; ES does.  We added one **without a new index type**: a generated BIGINT column
`geo_cell = s2_cell(lat, lon)` (S2 level 16, ~150 m) carries the stock numeric inverted (BKD) index; a new BE
predicate `s2_covers(poly, geo_cell)` turns a polygon into <= 64 sorted cell ranges and asks the BKD index one
closed range per cover range; an FE rewrite adds it in front of every eligible `ST_Contains`, so users change
no SQL.  On 100 M points a city polygon scans 3.3 % of rows, 10x faster than a full scan and faster than the
planar bbox prefilter -- which is also **wrong** (it misses 763 points because geodesic edges bulge).
Same core, three branches, all tests green.  The dominant lesson: **measure the existing machinery before
building new machinery** -- the planned BE index writer turned out to be one DDL line.

## 1. Statistics

### 1.1 Effort
| item | value |
|---|---|
| wall clock | ~15 h (12:00Z -> 02:51Z), one CLI session (claude-opus) + one chat orchestrator |
| commits on master branch `geo-index` | 40 (feature 4, test 4, opt/improvement 2, chore 2, plan/doc 28) |
| port commits | 4.1: 5, 3.1: 12 (3.1 needed more API adapters) |
| boxes used | c05 (dev, 32 cores); parallel phase added c07, c08 (build), c03, c17 (test clusters) |
| owner decisions | 9 (F1-F5 design, English-only plans, macOS waived, G4 relaxed, parallel boxes) |

### 1.2 Code (master branch, vs upstream `dbb9b19391`)
| area | files | lines added |
|---|---|---|
| core library `contrib/geo_index_core` (src + include + build) | 10 | 443 |
| BE (functions, BKD `BETWEEN_QUERY`, CMake) | 8 | 311 (-13) |
| FE (S2Cell, S2Covers, GeoIndexRewrite, registration) | 7 | 291 |
| tests (core gtest, BE UT, FE UT, regression suite) | 10 | 1,327 |
| test data (fixtures, expected output) | 4 | 9,609 |
| docs (plans, fix.archive, postmortems) | 17 | 1,757 |

**Product code ~1,045 lines; test code is 1.3x the product code.**  Ports: 34 files, ~11.8 k lines each
(mostly the same fixtures), adapter deltas only.

### 1.3 Quality
| item | value |
|---|---|
| audit rounds (independent model, `code-review`) | 10: design 1, core 3, BE adapter 6 |
| findings | 15 (10 HIGH, 5 MEDIUM); 15 fixed, 0 rejected |
| HIGH findings that were real bugs | find_package(s2) cannot work; BE not linked; wrong oracle (S2 vs Doris edge rule); DDL contradicting F1; 3 core bugs; 2 rewrite bugs; NULL cells under NOT/OR on 4.1/3.1 |
| tests | core gtest 11 cases + property + alloc + benchmark (ASan+UBSan, valgrind clean); BE UT 4 (ASAN); FE UT 3; regression `geo_index_p0` |
| correctness | indexed == unindexed `ST_Contains` on all suites, all three branches |

### 1.4 Performance (100 M points, 32 cores, all caches off)
| polygon | full scan | planar bbox (wrong) | index, cell-sorted | rows scanned | vs full | vs bbox |
|---|---|---|---|---|---|---|
| city | 2,033 ms | 274 ms | **204 ms** | 3.3 % | 10.0x | 1.34x |
| thin | 2,156 ms | 1,515 ms | **199 ms** | 4.6 % | 10.8x | 7.6x |
| concave | 2,285 ms | 727 ms | **607 ms** | 14.6 % | 3.8x | 1.20x |

Cost: load +16 % (bar 10 %, accepted), storage 1.33 -> 1.93 GB.  Core: `cell_of` 146 ns/point, `cover`
0.4-0.7 ms.  The BKD `BETWEEN_QUERY` alone took a cold city query from 575 ms to 105 ms.

## 2. Architecture as it now stands

```
SQL  ST_Contains(ST_Polygon(wkt), ST_Point(lon, lat))
 FE  GeoIndexRewrite (Nereids, pure pattern match, no S2 in Java)
       -> s2_covers(poly, geo_cell) AND ST_Contains(...)
 BE  s2_covers: poly -> geo_index::cover() -> <= 64 sorted cell ranges
       index path: one BKD BETWEEN per range -> row bitmap (approximate on master, exact on 4.1/3.1)
       row path:   cell in ranges (same answer without the index)
     ST_Contains: exact check on the candidates only
core contrib/geo_index_core: C++17 + S2 (already a Doris thirdparty on all 3 branches), no Doris headers
DDL  geo_cell BIGINT AS (s2_cell(lat, lon)), INDEX idx (geo_cell) USING INVERTED, optionally KEY(geo_cell)
```

Per branch: master uses approximate index results + `BETWEEN_QUERY`; 4.1 and 3.1 have no approximate
results, so the bitmap is exact ("cell in cover") with its own NULL bitmap, and use two one-sided BKD queries.

## 3. What went right

1. **Reuse over invention.**  The F1 decision ("cell column + existing 1-D BKD + exact ST_Contains") plus a
   measurement at 18:05Z showed that C.1 (writer) and C.2 (reader) needed no new BE index code.
2. **One library, three branches.**  `geo_index_core` was byte-identical on all branches; ports were adapters.
3. **Independent audits caught real bugs early**: 10 HIGH findings, including a NULL-semantics bug that only
   exists on 4.1/3.1 and that the regression suite then reproduced (18,531 vs 18,528 rows).
4. **Parallel boxes**: moving 3.1 to c07/c08 and test clusters to c03/c17 cut the port phase by ~2-3 h.

## 4. What went wrong / cost time

1. **A goal that measurement later showed unreachable.**  G4 "3x faster than bbox" -- for a compact polygon
   the bbox IS the scan's lower bound (~150 ms vectorised DOUBLE compare).  Settled by the owner (faster + exact).
2. **Profile overhead read as a real cost.**  "69 ms" became 51 ms with the profile off; a polygon cache
   (C.5) was nearly built for it.
3. **The `inverted_index_skip_threshold` trap.**  The stock BKD index is silently bypassed when it would
   select > 50 % -- first measurement scanned 87,411 of 100 k rows.  `s2_covers` sets `skip_try`.
4. **Cold build cost.**  A fresh branch = 2,620 compile units (1,415 BE + 1,064 BE test + generated) with an
   empty ccache; ~1.5 h per branch per box.
5. **Model id from memory** (`gpt-5.2`) -> 400 Unknown model on the first audit; R-AUDIT now forbids it.
6. **Orchestrator**: the 2 h reporting schedule was stopped by the orchestrator, not the owner (05:08Z report missed).

## 5. Critical findings worth remembering

- **Planar bbox is wrong for geodesic polygons**: city edge bulge 14.7 m; 763 of 2.73 M hits lost at 100 M rows.
  Any lat/lon-box prefilter (ES `geo_bounding_box`, hand-written `BETWEEN`) inherits this.
- **S2 id sign**: `id ^ (1 << 63)` keeps unsigned S2 order under a signed BIGINT, so `BETWEEN` is sound.
- **Doris `ST_Contains` excludes edge points** (`point_on_edge`); `S2Polygon::Contains` does not -- the test oracle
  must be Doris, not S2.
- **Sorting by `geo_cell`** turns each cover range into a contiguous row span: -24 % to -37 % time.
- **Exact vs approximate index results** change NULL semantics under NOT/OR; check every port for it.

## 6. Open items

| item | status |
|---|---|
| plan 02 G5 (load overhead <= 10 %) | +16 %, accepted by the owner; goal stays [4/5] |
| Z.3a interior cover (skip recheck for interior cells) | optional; decide on real polygon mix (checklist Q5) |
| production test | needs prod Doris version + deploy path from the owner |
| upstream PR to apache/doris | not started (push to upstream disabled by design) |
| next research | briefs 03 (altitude / stacked space), 04 (datums, GNSS gaps), 05 data checklist |

## 7. The transferable method

1. Ask the 5 design questions (F1-F5) before writing code; record the owner's answers in the plan.
2. Baseline first (Phase B), with a correctness oracle -- the baseline found the bbox bug.
3. For each new piece, measure whether the codebase already has it (KISS rung 2) before building.
4. Every code item gets an independent audit round; record findings with evidence and the fix commit.
5. When a goal is unreachable by measurement, show the bound and ask for a decision; do not silently relax it.

## Evidence
`fix.archive/01.geo.baseline.md`, `01.geo.index.reuse.md`, `01.geo.bench.md`, `01.geo.es.mapping.md`,
`02.geo.core.bench.md`, `02.audit.design.md`, `02.audit.core.md`, `02.audit.be-adapter.md`.
