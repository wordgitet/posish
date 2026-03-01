# Known Fails (M1)

This file tracks intentionally accepted failures for M1 so they do not get
misclassified as regressions.

## Accepted gaps

- `set -C` / noclobber behavior:
  - Current policy: option parses, behavior not fully enforced yet.
  - Impacted area: redirection semantics and related POSIX tests.
- Dollar-single-quote syntax (`$'...'`):
  - Current policy: parser recognizes and rejects with clear "not implemented"
    diagnostic.
  - Impacted area: quoting tests that require full escape processing.

## Signal status (updated 2026-02-28)

- Signal semantics use truth mode by default.
- Use `make test-signal` (`TESTCASE_TIMEOUT_SIGNAL=0`).
- Use `make test-signal-contained` for bounded per-file runs.

## Current active blockers (2026-02-28 current reruns)

- `kill4-p.tst` remains failing (no longer hard-aborting):
  - Symptom:
    - line 20 / 30: timeouts + missing `HUP` output, stderr includes
      `(: No such file or directory` and `kill: (-<pgid>): No such process`
    - line 44 / 61: timeouts + missing expected `USR1/USR2` or `TERM` output,
      stderr includes `halt(): No such file or directory`, `do: ...`, `done: ...`
  - Repro: `make -B -C tests/posix kill4-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=10`

## Maintenance

- Keep this list short and explicit.
- Remove entries as soon as implementation reaches conformance.
