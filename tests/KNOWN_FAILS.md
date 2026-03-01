# Known Fails

This file is the single source of truth for accepted M1 scope cuts and current
non-green POSIX test buckets.

## Snapshot (2026-03-01)

- Fulltruth baseline (`tests/posix/summary-fulltruth.csv`):
  - `FULL_PASS 95`
  - `PARTIAL_FAIL 27`
  - `FULL_FAIL 0`
  - `TIMEOUT 0`
  - `MISSING 0`
  - `OK 10719`
  - `ERROR 351`
  - `SKIPPED 14`
  - `RATIO_OK_OVER_OK_PLUS_ERROR 0.9683`

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
- `error-p.tst` (34)
- `cd-p.tst` (31)
- `read-p.tst` (29)
- `getopts-p.tst` (26)
- `tilde-p.tst` (21)
- `param-p.tst` (19)
- `case-p.tst` (13)
- `fsplit-p.tst` (13)
- `trap-p.tst` (13)
- `for-p.tst` (12)
- `builtins-p.tst` (10)
- `cmdsub-p.tst` (10)

The complete non-green file list is sourced from
`tests/posix/summary-fulltruth.csv`.

## Repro Commands

Use truth mode (`TESTCASE_TIMEOUT=0`) for semantic reruns.

```sh
make -B -C tests/posix alias-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix error-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix cd-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix read-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix getopts-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix tilde-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
make -B -C tests/posix param-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=0
```
