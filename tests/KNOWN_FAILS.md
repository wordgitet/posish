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

- Signal suites are currently green in the verified baseline.
- For signal-semantic truth runs, use `make test-signal`
  (`TESTCASE_TIMEOUT_SIGNAL=0`).
- For bounded execution with explicit timeout classification, use
  `make test-signal-contained`.
- Signal behavior remains active implementation work, but it is no longer an
  accepted broad-fail bucket in this tracker.

## Current active blockers (2026-02-28, builtins/params lane)

- Async/job-control edge cases:
  - `async-p.tst:27` (`asynchronous commands run asynchronously`)
  - `job-p.tst:21` (`stdin of asynchronous list is not modified with job control`)
  - `grouping-p.tst:34` and `grouping-p.tst:75` (async-list endings in grouped commands)
- Wait builtin gaps:
  - `wait-p.tst:76` (jobspec operands like `%echo`, `%exit`)
  - `wait-p.tst:101` (`while ... do ... done` control-flow coverage in trap-interrupt scenario)
- Kill-cluster blocker:
  - `kill4-p.tst` currently aborts (`make ... kill4-p.trs` exits with `Error 129`).

## Maintenance

- Keep this list short and explicit.
- Remove entries as soon as implementation reaches conformance.
