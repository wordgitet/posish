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
  - `command-p.tst:12` (`dot script not found does not kill shell`)
  - `kill4-p.tst` (abort path near testcase line 13, `sending signal to process 0`)
- Previously tracked async/grouping/wait blocker lines are now green in current
  reruns and were removed from this file.
