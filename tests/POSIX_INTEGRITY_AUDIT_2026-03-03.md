# POSIX Integrity Source Investigation (Code-First, De-Bandaid Focus)

Date: 2026-03-03  
Commit window: last 30 commits ending at `6daf175`  
Scope files: `src/exec.c`, `src/shell.c`, `src/expand.c`, `src/case_command.c`, `src/redir.c`, `src/builtins/special.c`, `src/builtins/regular.c`, `src/options.c`, `src/vars.c`

## Method

1. Freeze recent conformance window (`git log --oneline -n 30`).
2. Extract hotspot inventory and commit provenance (`git blame` + targeted line scans).
3. Map each finding to explicit POSIX clauses.
4. Build comparator evidence:
   - `posish`
   - `yash`
   - FreeBSD `bin/sh` subset (directional corpus)
   - `dash` for helper-function precedence checks not present in FreeBSD subset
5. Classify each finding:
   - `POSIX-Grounded`
   - `Likely POSIX`
   - `Ambiguous`
   - `Harness-Coupled`

## Evidence Index

Primary artifacts under `tmp/audit/2026-03-03/`:

- `commit_window.log`
- `file_commit_matrix.log`
- `file_commit_nameonly.log`
- `suspicious_tags.log`
- `hotspot_inventory.csv`
- `freebsd_target_matrix.csv`
- `freebsd_target_matrix_extra.csv`
- `freebsd_target_summary.txt`
- `helper_coupling_matrix.csv`
- per-case logs: `posish_*`, `yash_*`, `dash_*`

## Executive Summary

- Audited findings: 12
- `POSIX-Grounded`: 1
- `Likely POSIX`: 3
- `Ambiguous`: 5
- `Harness-Coupled`: 3
- High-risk findings (High / Medium-High): 9
- High-risk findings with comparator evidence: 9/9

### High-level assessment

1. `pipefail` is POSIX (Issue 8 / POSIX.1-2024) and is not treated as non-standard risk.
2. Most behavior remains clause-driven, but there are explicit helper-coupled shortcuts in `exec.c` + builtin dispatch.
3. The strongest de-bandaid risk is helper behavior leaking into default runtime semantics.

## Remediation Progress (In-Tree)

Status at 2026-03-03:

1. `Q1` implemented:
   - compile-time helper switch is wired in tracked build template (`Makefile.in`) as `POSISH_TEST_HELPERS` (default `0`).
2. `Q2` implemented:
   - helper-specific parser shortcuts and helper builtin visibility are now gated by `POSISH_TEST_HELPERS`.
3. `Q3` implemented:
   - `shell.c` and `exec.c` now use one completeness engine via `shell_needs_more_input_text_mode(..., include_heredoc)`.
   - fixed trailing-backslash completeness detection regression found during `alias-p.tst` recheck.
4. Remaining queue:
   - `Q4` alias-boundary revalidation,
   - `Q5` compatibility documentation.

## Findings

| ID | Commit | File:Line | Behavior Change | POSIX Clause(s) | Cross-Comparator Evidence | Classification | Risk | Recommendation |
|---|---|---|---|---|---|---|---|---|
| F01 | `67320c6` | `src/exec.c:1353-1359` | Function redefinition is ignored for `bracket`, `echoraw`, `make_command`. | XCU 2.9.1 Command Search and Execution (functions are searched before regular built-ins); XCU 2.10 Shell Grammar (function definition). | `helper_coupling_matrix.csv`: `helper_override_*` cases (`posish` differs; `yash` + `dash` allow function override). | Harness-Coupled | High | Gate helper-specific override bypass behind compile-time flag; default runtime must follow POSIX search order. |
| F02 | `67320c6` | `src/exec.c:2085-2101` | `ignore_helper_function_declaration()` skips `make_command()` declarations. | XCU 2.10 Shell Grammar (function definition parsing); XCU 2.3 Token Recognition. | `helper_coupling_matrix.csv`: `helper_decl_style_make_command` behavior deviates due parser shortcut path. | Harness-Coupled | High | Remove shortcut from default path and keep only under test-helper build flag. |
| F03 | `67320c6` | `src/exec.c:5127-5132` | `skip_next_done` chain bypasses normal parsing/execution after helper declaration pattern. | XCU 2.10 Shell Grammar (`for ... do ... done` structure); XCU 2.9 Shell Commands. | `helper_coupling_matrix.csv` + provenance from `git blame` line range. | Harness-Coupled | High | Eliminate helper-dependent control-flow bypass in default runtime. |
| F04 | `6daf175` | `src/shell.c:1626-1651` | Alias preview can override command-completeness decision in stream mode. | XCU 2.3/2.3.1 (token/alias substitution), XCU 2.9 command parsing. | `freebsd_target_matrix.csv`: alias/parser and shellproc cases are mixed; no direct clause-proof for this heuristic. | Ambiguous | Medium-High | Keep under review; move toward single grammar-driven completeness scanner. |
| F05 | `6daf175` | `src/shell.c:1689-1711` | Alias preview also drives EOF completeness path. | XCU 2.3, XCU 2.9, XCU 2.8 error handling. | Same evidence set as F04. | Ambiguous | Medium-High | Same treatment as F04; avoid alias-preview as authoritative parser gate. |
| F06 | `6daf175` + older | `src/shell.c:830+`, `src/exec.c:4620+` | Duplicate command-completeness scanners exist with overlapping but not identical logic. | XCU 2.3 Token Recognition; XCU 2.10 grammar consistency. | `freebsd_target_matrix.csv`: `execution/shellproc6.0` PASS but does not prove scanner equivalence. | Ambiguous | High | Unify into one authoritative completeness parser used by both shell stream and exec snippet paths. |
| F07 | `ca8a432` | `src/heredoc_command.c:531+`, `src/expand.c:2665+` | Heredoc expansion/error behavior still diverges on at least one external sample. | XCU 2.7.4 Here-Document; XCU 2.6 expansions. | `freebsd_target_matrix.csv`: `expansion/heredoc1.0` (`posish` FAIL, `yash` PASS), `parser/heredoc13.0` PASS on both. | Ambiguous | High | Keep as top POSIX semantic gap; constrain fixes to clause-backed heredoc rules. |
| F08 | `7f7ef16` + `ca8a432` | `src/exec.c:3053+`, `src/builtins/special.c:2382+` | Special-builtin redirection fatality/exit-status edges still inconsistent externally. | XCU 2.14 Special Built-In Utilities; XCU 2.8.1 consequences of errors; XCU 2.7 redirection. | `freebsd_target_matrix.csv`: both shells fail some redirection-error cases (`2.2`, `3.0`), pass `4.0`. | Likely POSIX | High | Consolidate one fatality path and rc mapping in remediation phase; not yash-tailored. |
| F09 | `d681b24` + `7f7ef16` | `src/builtins/special.c:2116+`, `src/exec.c:1655+` | Trap reset/clear inherited-ignore edges still have one comparator divergence. | XCU `trap`; XCU 2.14 special built-ins. | `freebsd_target_matrix.csv`: `trap10.0` diverges (`posish` FAIL, `yash` PASS), `trap16.0` both FAIL, `trap17.0` both PASS. | Likely POSIX | High | Keep POSIX intent; tighten transition semantics with explicit signal-state model. |
| F10 | `540f69c` | `src/options.c:23`, `src/exec.c:4028` | `pipefail` implemented and pipestatus aggregation snaps option at pipeline start. | POSIX.1-2024 Issue 8 `set -o pipefail`; Austin Group defect resolution. | `freebsd_target_matrix_extra.csv`: `execution/pipefail1.0` and `pipefail2.42` PASS on both. | POSIX-Grounded | Medium | Keep as normative behavior; add compatibility note for older environments. |
| F11 | `fe2641e` + `6daf175` | `src/exec.c:5125` and line-base plumbing | `LINENO` physical-line accounting still mismatches external samples. | XCU `sh` shell variable semantics (`LINENO`). | `freebsd_target_matrix.csv`: `builtins/lineno.0` and `lineno2.0` fail for both shells, with different rc details. | Ambiguous | Medium | Rework line-origin bookkeeping only with clause-backed acceptance matrix. |
| F12 | `cc63462` | `src/builtins/regular.c:184+` | `cd` mostly aligned but still misses one directional external case. | XCU `cd` utility (`-L/-P`, `CDPATH`, output/update semantics). | `freebsd_target_matrix_extra.csv`: `cd1.0` PASS on both; `cd9.0` (`posish` FAIL, `yash` PASS). | Likely POSIX | Medium-High | Fix specific clause-backed `cd9.0` edge in remediation phase. |

## Safe Confidence List

1. `pipefail` behavior should remain as POSIX Issue 8 semantics (`freebsd_target_matrix_extra.csv` shows parity for sampled cases).
2. `set -e` suppression context fixes remain stable in sampled non-yash checks (`set-e/if1.0`, `set-e/while1.0`, `set-e/and1.0`, `set-e/subshell2.1` in `freebsd_target_matrix.csv`).
3. Current queue should preserve these areas while removing helper-coupled parser/runtime shortcuts.

## Decision-Complete Remediation Queue (Report-Only)

This queue follows the locked order from the investigation plan.

| Queue ID | Priority | Target | Current Behavior | Desired Behavior | Patch Surface | Acceptance Checks | Rollback Scope |
|---|---|---|---|---|---|---|---|
| Q1 | P0 | Isolate helper-only behavior behind compile-time switch | Helper builtins and helper parse shortcuts always active in default runtime. | Default build (`POSISH_TEST_HELPERS=0`) must not expose helper semantics; helper mode (`=1`) preserves harness compatibility. | `Makefile`, `src/builtins/regular.c`, `src/exec.c`, optional header macro centralization. | 1) `helper_coupling_matrix.csv` re-run: default build should match `yash`/`dash` function override behavior. 2) POSIX guard suites green. 3) Helper-enabled run keeps harness compatibility. | One commit revert for compile-time gating. |
| Q2 | P0 | Remove helper parser shortcuts from default runtime path | `ignore_helper_function_declaration` + `skip_next_done` alter parser/executor control flow. | Default path uses normal grammar/parsing only; helper shortcuts compile in only under helper flag. | `src/exec.c` (functions around lines ~2085 and ~5127). | 1) Targeted helper-decl tests in both flag modes. 2) `make test-smoke`, `make test-regressions`, `make test-stop`, `make test-signal`. | One commit revert for parser-shortcut removal. |
| Q3 | P1 | Unify command-completeness scanner | Two scanners (`shell.c` and `exec.c`) can drift and yield different “need more input” results. | Single authoritative scanner used by both stream chunking and exec-snippet paths. | `src/shell.c`, `src/exec.c`, shared helper extraction. | 1) Re-run `input-p`, `comment-p`, alias parser subset. 2) FreeBSD directional checks: `execution/shellproc6.0`, alias/parser cases. 3) Full guard suites green. | One commit revert for scanner unification. |
| Q4 | P1 | Re-validate alias boundary logic without helper coupling | Alias preview currently influences completeness heuristics directly. | Alias expansion remains semantic, but parser completeness remains grammar-driven and independent from helper hacks. | `src/shell.c`, `src/exec.c` alias preview integration points. | 1) `alias-p.trs` + targeted parser alias cases. 2) FreeBSD alias cases in matrix. 3) Guard suites green. | One commit revert for alias-boundary revision. |
| Q5 | P2 | Keep `pipefail` as POSIX behavior and document baseline | Some prior notes treated `pipefail` as extension risk. | Runtime unchanged; docs explicitly mark Issue 8 POSIX status and compatibility note for older systems. | Docs only (no runtime code). | Re-run pipeline directional checks and regression guards; no behavior delta expected. | Revert docs-only commit if wording changes are unwanted. |

## Remediation Execution Policy (Next Phase)

1. One commit per queue item (`Q1`..`Q5`).
2. Mandatory guards after each commit:
   - `make test-smoke`
   - `make test-regressions`
   - `make test-stop`
   - `make test-signal`
3. If any regression appears, revert only the affected queue item commit and rework.

## Reproduction Commands

```sh
# 1) Freeze commit window
git log --oneline -n 30 > tmp/audit/2026-03-03/commit_window.log

# 2) Hotspot scans
rg -n "ignore_helper_function_declaration|skip_next_done|make_command|echoraw|bracket|needs_more_input|alias_preview|pipefail" \
  src/exec.c src/shell.c src/builtins/regular.c src/options.c \
  > tmp/audit/2026-03-03/suspicious_tags.log

# 3) Helper coupling matrix (posish vs yash vs dash)
# (already generated for this report)
cat tmp/audit/2026-03-03/helper_coupling_matrix.csv

# 4) External directional matrices
cat tmp/audit/2026-03-03/freebsd_target_matrix.csv
cat tmp/audit/2026-03-03/freebsd_target_matrix_extra.csv
```

## POSIX References

- Shell Command Language (Issue 7):  
  `https://pubs.opengroup.org/onlinepubs/9699919799/utilities/V3_chap02.html`
- `sh` utility variables (`LINENO`):  
  `https://pubs.opengroup.org/onlinepubs/9699919799/utilities/sh.html`
- `set` utility (Issue 8 / POSIX.1-2024):  
  `https://pubs.opengroup.org/onlinepubs/9799919799.2024edition/utilities/set.html`
- `trap` utility:  
  `https://pubs.opengroup.org/onlinepubs/9699919799/utilities/trap.html`
- `cd` utility:  
  `https://pubs.opengroup.org/onlinepubs/9699919799/utilities/cd.html`

## Final Assessment

This was not “just patching to pass yash tests.” Most behavior is POSIX-intent, but there are real helper-coupled shortcuts in `exec.c` + builtin dispatch that can leak into normal runtime semantics.  
The remediation queue is now decision-complete and prioritized to remove those shortcuts first while preserving current conformance gains.
