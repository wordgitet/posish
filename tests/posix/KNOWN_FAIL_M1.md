# Known Failures (M1)

This file tracks intentional M1 gaps so summary metrics remain actionable.

## Signal / Harness Truthfulness

Status: `active runbook guidance`

Notes:

- For signal-semantic validation, run with `TESTCASE_TIMEOUT=0`.
- Use `make test-signal` for truthful signal semantics.
- Use `make test-signal-contained` when you need guaranteed completion with
  explicit PASS/FAIL/TIMEOUT classification per file.

## Current M1 Buckets

Status: `active`

Notes:

- Active blockers (from 2026-02-28 fulltruth rebaseline):
  - `kill4-p.tst` remains failing (no longer aborting at line 13):
    - line 20 / 30: timeout + missing `HUP` with stderr `(: No such file or directory`
      and `kill: (-<pgid>): No such process`
    - line 44 / 61: timeout + missing expected job-signal output with stderr
      `halt(): No such file or directory`, `do: ...`, `done: ...`
- Previously tracked async/grouping/wait blocker lines are now green in current
  reruns and were removed from this file.
