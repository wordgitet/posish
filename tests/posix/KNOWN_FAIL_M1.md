# Known Failures (M1)

This file tracks intentional M1 gaps so summary metrics remain actionable.

## Signal / Harness Truthfulness

Status: `active runbook guidance`

Notes:

- For signal-semantic validation, run with `TESTCASE_TIMEOUT=0`.
  Non-zero testcase timeout can distort outcomes in `sigint*` / `sigquit*`.
- Use `make test-signal` for truthful signal semantics.
- Use `make test-signal-contained` when you need guaranteed completion with
  explicit PASS/FAIL/TIMEOUT classification per file.

## Current M1 Buckets

Status: `active`

Notes:

- Green confirmations retained:
  - `sigtstp4-p.tst`
  - `sigttin4-p.tst`
  - `sigttou4-p.tst`
  - `command-p.tst:253` (`describing alias (-V)`)
- Remaining blockers in current lane:
  - `async-p.tst:27`
  - `job-p.tst:21`
  - `grouping-p.tst:34`, `grouping-p.tst:75`
  - `wait-p.tst:76`, `wait-p.tst:101`
  - `kill4-p.tst` (`Error 129` abort path)
