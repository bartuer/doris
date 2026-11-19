# Dev process -- geo-index fork (bartuer/doris, branch geo-index)

Local process rules for the geo index work.  AGENTS.md (apache/doris) still
applies in full; these rules add to it.  This file, CLAUDE.md and
`.github/REPL/` are fork-local: strip them from any PR to apache/doris.

## R-ENGLISHPLAN
Plan docs, fix.archive notes, commit messages and code comments are English
only.  Before committing a plan: `grep -nP '[\x{3000}-\x{9fff}\x{ff00}-\x{ffef}]' <file>`
prints nothing.

## R-KISS -- every design passes /kiss before code
- Before a work item writes code, run the `kiss` skill in audit mode on its
  design (the plan item + the files it will touch).  Walk the ladder:
  needed at all? -> already in Doris? -> std? -> platform? -> an installed
  dependency (S2, CLucene BKD, gtest)? -> one expression? -> only then new code.
- Record the result under the item as `:kiss: <rung reached; what was removed>`.
  A design with no `:kiss:` line is not ready to implement.
- Order of preference (owner, F1): simplest and most compatible first
  (builds on master, branch-4.1, branch-3.1), then efficiency.

## R-AUDIT -- every code change is audited by a separate agent
- After the `:test_tool:` passes and before flipping `[X]`, a SEPARATE agent
  reviews the diff: the `code-review` agent (or the `security-review` agent for
  trust-boundary code), run on a DIFFERENT model from the author's.
- Audit models (owner 2026-11-19T14:01Z: "use gpt-6.1-astra (ghe) or claude-sonnet-55"; MEASURED the same
  minute: those exact ids return 400 Unknown model; the matching ids that answer are):
    - `gpt-6-astra`        (first choice)
    - `claude-sonnet-5-5`  (second choice; use it when the author IS gpt-6-astra)
  `task(agent_type="code-review", model="gpt-6-astra")`.  Never type a model id from memory: when a
  listed id returns 400, take the closest id from the tool's "Available:" list and record the substitution.
- The author fixes or answers every finding.  Record the result under the item as
  `:audit: <agent>/<model> <n findings, n fixed, n rejected + why>`.
- No `[X]` on a code item without an `:audit:` line.

## R-MEMCHECK -- C++ core is memory-clean
- The geo index core (plan 02) builds standalone and its unit tests run
  under ASan+UBSan AND under `valgrind --error-exitcode=1 --leak-check=full`
  in the plan-02 Docker image.  Both must be clean before `[X]`.
- Inside Doris, BE UT runs with the default ASAN build type (AGENTS.md).

## R-TESTDATA -- tests are checked in and run locally
- Test data (points, polygons, expected row ids) lives in the repo next to the
  test, small (<= 1 MB per file) and deterministic (fixed seed + generator script
  checked in).  Larger bench data is generated, never committed.
- Any Linux or macOS machine runs the core tests with one command
  (plan 02 `:test_tool:`); Docker is the reference environment.

## R-ROOM
Work for plan 01/02 is claimed and finished in room `doris`
(`room -c doris ...`): ack to claim, progress per `[ ]->[X]`, done/failed with
the commit sha.  Never push to apache/doris; push to origin (bartuer/doris).
