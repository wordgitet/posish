# Known Fails

This file is the single source of truth for accepted M1 scope cuts and current
non-green POSIX test buckets.

## Snapshot (2026-03-01)

- Fulltruth baseline (`tests/posix/summary-fulltruth.csv`):
  - `FULL_PASS 88`
  - `PARTIAL_FAIL 34`
  - `FULL_FAIL 0`
  - `TIMEOUT 0`
  - `MISSING 0`
  - `OK 10504`
  - `ERROR 566`
  - `SKIPPED 14`
  - `RATIO_OK_OVER_OK_PLUS_ERROR 0.9489`

## Accepted Scope Cuts

- `set -C` / noclobber behavior:
  - Current policy: option parses, behavior not fully enforced yet.
  - Impacted area: redirection semantics and related POSIX tests.
- Dollar-single-quote syntax (`$'...'`):
  - Current policy: parser recognizes and rejects with clear "not implemented"
    diagnostic.
  - Impacted area: quoting tests that require full escape processing.

## Current Blockers / Regressions

Top active non-green buckets by error count:

- `alias-p.tst` (58)
- `option-p.tst` (53)
- `param-p.tst` (45)
- `error-p.tst` (37)
- `quote-p.tst` (32)
- `read-p.tst` (31)
- `if-p.tst` (31)
- `set-p.tst` (25)
- `getopts-p.tst` (26)
- `redir-p.tst` (21)
- `ppid-p.tst` (1)

The complete non-green file list is sourced from
`tests/posix/summary-fulltruth.csv`.

## Repro Commands

Use truth mode (`TESTCASE_TIMEOUT=0`) for semantic reruns.

```sh
make -B -C tests/posix option-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix set-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix shift-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix redir-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix param-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix ppid-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
```
