# posish

`posish` is a work-in-progress POSIX shell implementation in C11.

Current state:
- Compilable shell core with iterative POSIX parser/executor implementation.
- POSIX test harness vendored from yash (`*-p.tst` only).
- Autoconf-based build configuration with portable makefiles.

## Configure and Build

```sh
autoreconf -fi
./configure
make
```

If the generated configure cache points at a stale or host-inappropriate
compiler, override `CC` explicitly for local builds:

```sh
make -B all CC=cc
```

Enable tracing hooks at configure time (disabled by default):

```sh
./configure --enable-trace
```

Binary:

```sh
./build/posish
```

## Android (Termux)

`posish` can be built on Termux using `gcc` without committing Android-specific
paths into generated files.

```sh
pkg install build-essential autoconf automake
autoreconf -fi
CC=gcc sh ./configure --prefix="$PREFIX"
make -j"$(nproc)"
make install
```

## Run Tests

If `yash` is not discoverable at configure time, pass `YASH_RUNNER` explicitly:

```sh
make test-smoke CC=cc YASH_RUNNER=/absolute/path/to/yash
make test-posix CC=cc YASH_RUNNER=/absolute/path/to/yash
make test-stop CC=cc YASH_RUNNER=/absolute/path/to/yash
make test-signal CC=cc YASH_RUNNER=/absolute/path/to/yash
make test-regressions CC=cc YASH_RUNNER=/absolute/path/to/yash
make rebaseline-fulltruth CC=cc YASH_RUNNER=/absolute/path/to/yash
```

Signal-specific notes:
- `test-signal` uses `TESTCASE_TIMEOUT_SIGNAL=0` by default to avoid
  per-testcase timeout distortion of signal semantics.
- `test-signal-contained` adds an outer file timeout and always reports
  `PASS/FAIL/TIMEOUT` per signal test file.

```sh
make test-signal-contained
```

Full-suite pure-truth rebaseline (outer per-file timeout + checklist sync):

```sh
make rebaseline-fulltruth
```

Artifacts are written to `tests/posix/`:
- `summary-fulltruth.log`
- `summary-fulltruth.csv`
- `summary-fulltruth.full-pass.list`
- `summary-fulltruth.partial-fail.list`
- `summary-fulltruth.full-fail.list`
- `summary-fulltruth.timeout.list`
- `summary-fulltruth.missing.list`

Status meanings in `summary-fulltruth.csv`:
- `FULL_PASS`: command returned success and `ERROR=0`.
- `PARTIAL_FAIL`: both `OK>0` and `ERROR>0`.
- `FULL_FAIL`: no passing assertions for that file or non-zero command with parsed failures.
- `TIMEOUT`: outer file-level timeout reached.
- `MISSING`: result file was not produced.

Or explicitly set a testee path:

```sh
make test-posix TESTEE=/absolute/path/to/shell
```

`test-posix` requires system `yash` as the runner for `run-test.sh`.
If missing, install the latest yash (recommended: build from source) or set:

```sh
make test-posix CC=cc YASH_RUNNER=/absolute/path/to/yash
```

Append pass-rate metrics to `tmp/metrics/posix.csv`:

```sh
make metrics
```

## Local Notes

Some local runs can leave behind permissioned `tests/posix/tmp.*` artifacts.
If `make clean` fails because of those directories, prefer forcing a rebuild:

```sh
make -B all CC=cc
```

## Startup Behavior

`posish` currently uses user-scoped startup files only.
No system-wide startup files such as `/etc/profile` are loaded yet.

Startup policy:

- interactive shell:
  - loads `ENV` if set
  - then loads `~/.posishrc` if `HOME` is set
- login shell:
  - loads `~/.posish_profile` if `HOME` is set
- interactive login shell:
  - loads `~/.posish_profile`
  - then `ENV`
  - then `~/.posishrc`
- non-interactive `-c` and script execution:
  - load no startup files
- non-interactive login shell:
  - loads `~/.posish_profile` only

Login shell detection follows the usual `argv[0][0] == '-'` convention.

Prompt defaults remain interactive-only:

- `PS1='\w \$ '` when unset
- `PS2='> '` when unset

Shell-owned variable baseline:

- `IFS` is initialized to the default `<space><tab><newline>` baseline.
- `OPTIND` starts at `1` and `OPTARG` starts unset.
- `PATH`, `HOME`, `PWD`, `OLDPWD`, and `ENV` are preserved from the parent
  environment unless normal shell execution updates them.

## Allocator Policy

`posish` uses a real arena allocator for runtime ownership control:
- `arena_perm`: process lifetime
- `arena_script`: per top-level script/program
- `arena_cmd`: per command/snippet

Runtime code should allocate through allocator wrappers instead of direct
`malloc`/`realloc`/`free`:
- `arena_xmalloc`, `arena_xrealloc`, `arena_xstrdup`
- `arena_alloc_in`, `arena_realloc_in`, `arena_maybe_free`

Hybrid behavior is intentional:
- `arena == NULL` in `arena_alloc_in`/`arena_realloc_in` means plain heap
  semantics via wrappers.
- `arena_maybe_free` frees only non-arena pointers and no-ops for arena-owned
  pointers.

Direct raw allocation calls are intentionally limited to:
- `src/arena.c` allocator internals

When built with `--enable-trace`, runtime tracing is controlled by:

```sh
POSISH_TRACE=signals,jobs,traps
```

## License

- Shell implementation in this repository: 0BSD (`LICENSE`).
- Vendored third-party test files under `tests/posix/` keep their original
  upstream licenses. See `THIRD_PARTY_NOTICES.md`.
