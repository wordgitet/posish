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

## Current active blockers (2026-02-28 fulltruth)

- `command-p.tst:12` (`dot script not found does not kill shell`):
  - Symptom: expected output line `reached` is missing.
  - Repro: `make -B -C tests/posix command-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=10`
- `kill4-p.tst` abort path:
  - Symptom: harness aborts early at testcase line 13 (`sending signal to process 0`).
  - Repro: `make -B -C tests/posix kill4-p.trs TESTEE=../../build/posish TESTCASE_TIMEOUT=10`

## Maintenance

- Keep this list short and explicit.
- Remove entries as soon as implementation reaches conformance.
