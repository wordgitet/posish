# Posish Internal Roadmap

This is an internal engineering roadmap for `posish`.

- It may use frank language like "Hall of Shame".
- It may track local-machine benchmark baselines.
- It is not a standards document.
- It is not a user-facing guarantee.

## Current Snapshot

- Release line: `v0.1.2`
- Imported POSIX suite is green.
- Current truth snapshot from the latest tty-source-of-truth run:

```text
TOTAL:   10917
OK:      10903
ERROR:       0
SKIPPED:    14
```

- `%REQUIRETTY%` behavior should still be debugged from a real terminal session when there is any doubt.
- Current focus is no longer "make tests pass"; current focus is "make `posish` feel like a real shell and stop being slow".

## What Just Shipped

- Job-control and jobspec correctness work landed.
- Arithmetic invalid-shift handling was hardened.
- `pwd` shell behavior was fixed.
- Command substitution `EXIT` trap behavior was fixed.
- Imported POSIX conformance was brought back to green after follow-up regressions.
- `v0.1.2` was released after the conformance fixes were verified.

## Release Horizon

- This roadmap covers the next 3 releases.
- Target releases:
  - `v0.1.3`
  - `v0.1.4`
  - `v0.1.5`
- Prioritization principle:
  - correctness and shell identity first
  - then maturity, documentation, and structure
  - then performance floor-raising
  - larger architecture work stays parked unless it directly unblocks a release goal

## Hall of Shame: Shellbench Baseline

This is the current embarrassment baseline.

- Higher is better.
- This table exists for motivation and trend tracking.
- Treat it as same-machine directional data, not universal cross-host truth.

Baseline metadata:

- Date: `2026-03-05`
- Status: `baseline imported from local shellbench run`
- Interpretation: `posish` is materially slower than peer shells on many builtin-heavy cases.
- Goal style: close major gaps first, not immediate parity with `dash`.

```text
----------------------------------------------------------------------------------------------
name                                               /home/mario/proj/posish/build/posish       dash freebsd-sh       yash
----------------------------------------------------------------------------------------------
assign.sh: positional params                          182,790  2,150,031  2,431,920    324,602
assign.sh: variable                                   195,195  2,862,863  3,090,974    388,507
assign.sh: local var                                    error  2,880,470  3,012,449    392,584
assign.sh: local var (typeset)                          error      error      error    394,277
cmp.sh: [ ]                                            56,117  1,218,361  2,018,507    144,414
cmp.sh: [[ ]]                                           error      error      error    276,052
cmp.sh: case                                          159,666  2,779,571  3,474,010    357,938
count.sh: posix                                       170,976  1,985,485  2,164,603    337,550
count.sh: typeset -i                                    error      error      error      error
count.sh: increment                                     error      error      error      error
eval.sh: direct assign                                 96,843  1,867,096  1,952,761    177,146
eval.sh: eval assign                                   74,025  1,110,132  1,201,106    133,803
eval.sh: command subs                                     974      5,675      5,232      4,066
func.sh: no func                                      219,571  2,927,598  3,169,871    374,688
func.sh: func                                         129,164  2,214,775  2,475,419    211,938
null.sh: blank                                        326,640      error  4,065,115    443,796
null.sh: assign variable                              194,909  2,853,106  3,130,796    404,558
null.sh: define function                              201,168  3,000,502  3,336,059    393,022
null.sh: undefined variable                           186,169  2,825,985  3,300,957    387,364
null.sh: : command                                    216,225  2,905,601  3,175,414    373,020
output.sh: echo                                         1,349  1,033,847  1,112,369    160,081
output.sh: printf                                       1,336  1,001,659  1,095,769    157,458
output.sh: print                                        error      error      error      error
stringop1.sh: string length                           169,400  2,271,920  2,831,770    374,766
stringop2.sh: substr 1 builtin                        167,537      error      error      error
stringop2.sh: substr 1 echo | cut                         755      1,050      1,001      1,230
stringop2.sh: substr 1 cut here doc                       883      1,342      1,386      1,021
stringop2.sh: substr 1 cut here str                     error      error      error      1,030
stringop2.sh: substr 2 builtin                        163,078      error      error      error
stringop2.sh: substr 2 echo | cut                         753      1,054        988      1,220
stringop2.sh: substr 2 cut here doc                       882      1,332      1,386      1,026
stringop2.sh: substr 2 cut here str                     error      error      error      1,037
stringop2.sh: substr 2 expr                               898      1,231      1,589      1,169
stringop3.sh: str remove ^ shortest builtin           161,531  2,206,721  2,430,589    350,835
stringop3.sh: str remove ^ shortest echo | cut            748      1,053        987      1,211
stringop3.sh: str remove ^ shortest cut here doc          875      1,337      1,384      1,017
stringop3.sh: str remove ^ shortest cut here str        error      error      error      1,012
stringop3.sh: str remove ^ longest builtin            156,133  1,917,828  2,217,117    346,646
stringop3.sh: str remove ^ longest echo | cut             736      1,048        990      1,220
stringop3.sh: str remove ^ longest cut here doc           872      1,337      1,386      1,021
stringop3.sh: str remove ^ longest cut here str         error      error      error      1,024
stringop3.sh: str remove $ shortest builtin           164,912  2,243,186  2,581,727    350,833
stringop3.sh: str remove $ shortest echo | cut            747      1,061        994      1,220
stringop3.sh: str remove $ shortest cut here doc          889      1,353      1,385      1,021
stringop3.sh: str remove $ shortest cut here str        error      error      error      1,007
stringop3.sh: str remove $ longest builtin            163,909  2,214,120  2,601,377    348,949
stringop3.sh: str remove $ longest echo | cut             740      1,045        995      1,217
stringop3.sh: str remove $ longest cut here doc           881      1,330      1,374      1,026
stringop3.sh: str remove $ longest cut here str         error      error      error      1,006
stringop4.sh: str subst one builtin                   169,267      error      error    326,511
stringop4.sh: str subst one echo | sed                    657        820        777        911
stringop4.sh: str subst one sed here doc                  707        996        997        805
stringop4.sh: str subst one sed here str                error      error      error        801
stringop4.sh: str subst all builtin                   168,075      error      error    325,587
stringop4.sh: str subst all echo | sed                    655        817        770        914
stringop4.sh: str subst all sed here doc                  707        995        998        798
stringop4.sh: str subst all sed here str                error      error      error        797
stringop4.sh: str subst front builtin                 168,490      error      error    333,768
stringop4.sh: str subst front echo | sed                  650        817        767        903
stringop4.sh: str subst front here doc                    690        991        996        790
stringop4.sh: str subst front sed here str              error      error      error        787
stringop4.sh: str subst back builtin                  165,284      error      error    331,119
stringop4.sh: str subst back echo | sed                   655        800        778        905
stringop4.sh: str subst back here doc                     700        989        998        802
stringop4.sh: str subst back sed here str               error      error      error        791
subshell.sh: no subshell                              200,739  2,672,810  3,011,718    188,716
subshell.sh: brace                                    156,868  2,693,842  2,960,972    162,875
subshell.sh: subshell                                   4,017      6,063      5,366      4,108
subshell.sh: command subs                               3,497      5,748  2,230,782      4,426
subshell.sh: external command                           1,692      2,420      2,468      1,797
----------------------------------------------------------------------------------------------
* count: number of executions per second
```

- Benchmark is internal-only and allowed to be a little mean.
- Benchmark score is executions per second.
- Compare revisions on the same machine.
- Do not use this table as a release blocker by itself.
- Use it to find catastrophic hotspots and verify improvement.

Canonical future rerun command:

```sh
shellbench -c -s "$PWD/build/posish,dash,freebsd-sh,yash" tmp/external/shellbench/sample/*.sh
```

If `shellbench` is not on `PATH`, the local checkout under `tmp/external/shellbench` is an acceptable source.

Hotspot buckets:

- `Critical outliers`
  - `output.sh: echo`
  - `output.sh: printf`
  - `eval.sh: command subs`
  - other cases scoring in the low thousands or below
- `Builtin-heavy slow path`
  - assignment, comparison, count, null, and string operation benchmarks
- `Structural overhead`
  - function dispatch
  - expansion cost
  - shell startup, dispatch, and command evaluation overhead
- `Lower priority`
  - cases dominated by external-process cost

## v0.1.3 - Shell Identity

Scope: make `posish` behave more like a real shell at startup and in interactive use.

Included work:

### A. POSIX environment variable audit and fixes

Audit and normalize shell-owned variables and shell startup interactions for at least:

- `PATH`
- `HOME`
- `IFS`
- `PWD`
- `OLDPWD`
- `PS1`
- `PS2`
- `ENV`
- `LINENO`

For each variable, document and then implement:

- whether `posish` initializes it
- when it is imported from the parent environment
- when shell startup logic overwrites it
- whether it is exported
- whether behavior differs in interactive and non-interactive mode

### B. Startup behavior matrix

Define, document, and implement a startup matrix covering:

- interactive shell on tty
- non-interactive script execution
- `-c` command execution
- login-shell invocation
- shell invoked as `-posish`
- shell invoked with `-i`
- shell reading from stdin without tty
- shell startup with `ENV` set

This release must document the matrix, not just code it.

### C. Prompt behavior

Remove hardcoded unconditional `PS1="posish$"` behavior.

Required target behavior:

- only set a default `PS1` for interactive shells
- only set it if `PS1` is unset
- do not force a prompt in non-interactive mode
- keep the default simple for now

Recommended default:

```sh
PS1='$ '
PS2='> '
```

If project identity later wants `posish$ ` back, revisit that after startup semantics are stable.

Acceptance criteria:

- startup behavior is written down as a matrix
- shell-owned environment variable policy is documented
- default prompt is no longer hardcoded unconditionally
- interactive and non-interactive behavior is intentional and testable
- imported POSIX suite remains green

Test scenarios:

- interactive tty shell with no `PS1`
- interactive tty shell with inherited `PS1`
- non-interactive `-c` shell does not emit a prompt
- login-shell path does not regress batch behavior
- `ENV` handling in interactive and non-interactive modes
- `PWD` and `OLDPWD` consistency across `cd`

## v0.1.4 - Project Maturity

Scope: turn correctness into maintainability and usability.

Included work:

### A. Detailed man page

Add a real `posish(1)` man page with these sections:

- `NAME`
- `SYNOPSIS`
- `DESCRIPTION`
- `INVOCATION`
- `OPTIONS`
- `COMMAND EXECUTION MODEL`
- `BUILTINS`
- `EXPANSIONS`
- `REDIRECTIONS`
- `ENVIRONMENT`
- `EXIT STATUS`
- `JOB CONTROL`
- `CONFORMANCE / NON-GOALS`
- `BUGS`

The man page is user-facing and must avoid local-machine notes.

### B. Error handling structure

Introduce a coherent internal error-handling layer.

Target:

- dedicated header at `src/error.h`

Taxonomy to define:

- syntax error
- expansion or runtime error
- builtin usage error
- fatal internal or system error

Behavior to document:

- consistent diagnostic formatting rules
- which errors affect command status
- which errors affect shell status
- which errors terminate the shell

### C. Targeted refactoring

Keep refactoring targeted, not vague.

Priority refactor areas:

- execution path complexity in `src/exec.c`
- expansion and command-substitution flow in `src/expand.c`
- builtin classification and dispatch consistency

Acceptance criteria:

- a committed man page exists
- error handling architecture is documented and partially implemented
- at least one high-complexity subsystem is structurally simplified
- imported POSIX conformance remains green

Validation scenarios:

- error message consistency for builtin misuse
- syntax versus runtime failure status behavior
- command substitution diagnostics remain correct
- man page examples match actual runtime behavior

## v0.1.5 - Benchmaxxing

Scope: make performance an explicit project track without sacrificing correctness.

Strategic goal:

- Do not chase first place yet.
- Get `posish` out of the basement on obvious builtin-heavy benchmarks.

Included work:

### A. Benchmark harness normalization

Create a repeatable internal benchmark workflow.

Planned helper:

- `tools/bench/run-shellbench.sh`

This script should:

- build or point at `./build/posish`
- run the canonical shellbench command
- print timestamp and host note
- optionally save output to a dated file under `benchmarks/` or `docs/benchmarks/`

### B. Hotspot investigation order

Optimization order:

1. shell startup and dispatch overhead for trivial commands
2. builtin output path
   - `echo`
   - `printf`
3. variable assignment and parameter lookup fast paths
4. expansion-heavy string operations
5. function and `eval` dispatch cost
6. command substitution and subshell overhead

### C. Success metric

Close major gaps first.

Define success for `v0.1.5` as:

- achieve at least one order-of-magnitude improvement on the worst builtin-heavy outliers from the baseline
- eliminate obviously pathological low-thousands scores for simple builtin cases where peer shells are in the hundreds of thousands or millions
- keep the imported POSIX suite green throughout optimization work

This is intentionally not "beat `dash`".

Acceptance criteria:

- shellbench workflow is committed and repeatable
- the Hall of Shame section gets a before and after snapshot
- at least 3 critical outlier benchmark families improve materially
- no correctness regression appears in guard suites

Validation scenarios:

- rerun the canonical shellbench command on the same machine
- rerun:
  - `make test-smoke`
  - `make test-regressions`
  - `make test-stop`
  - `make test-signal`
  - `make test-posix`
- compare only against the local baseline and the immediate previous run

## Parking Lot After v0.1.5

These items matter, but they are outside the next 3-release core path.

### A. Full AST implementation

- High-value long-term architecture work.
- Not urgent while imported POSIX conformance is green.
- Start it only when the project is ready for major parser and executor churn.

### B. Line editing

- Desirable user-facing improvement.
- Should come after startup and prompt semantics are stable.
- Prefer `libedit` first, not a handmade editor, unless there is a deliberate reason to own the whole terminal stack.

### C. Broad refactors not tied to a release goal

- Keep these out of scope unless they directly unblock one of the releases above.

## Release Gates

Every release in this roadmap must satisfy all of the following:

- imported POSIX suite stays green
- `test-smoke`, `test-regressions`, `test-stop`, and `test-signal` stay green
- any new behavior has committed tests or committed docs
- benchmark improvements do not count if they break conformance
- roadmap sections are updated when a release lands

## Benchmark Update Policy

- Only update Hall of Shame numbers after a meaningful benchmark run.
- Keep the original baseline table and append new snapshots below it; do not silently replace history.
- Annotate each snapshot with:
  - date
  - commit
  - command
  - machine note
- Do not compare different machines as if they were the same baseline.
- Benchmark deltas are advisory, not a sole release gate.

## Notes and Assumptions

- `REVIEW_ROADMAP.md` is the canonical committed internal roadmap.
- "Hall of Shame" is acceptable project-internal wording.
- The shellbench table above is the first committed benchmark baseline.
- Future canonical shellbench runs should use correction mode and the local sample suite.
- The roadmap horizon is exactly `v0.1.3`, `v0.1.4`, and `v0.1.5`.
- Success for performance work means closing major gaps first, not trying to beat `dash` immediately.
- Full AST and line editing are intentionally parked outside the next 3-release core path.
- This roadmap is docs-only and does not imply that every planned interface already exists.
