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

Enable tracing hooks at configure time (disabled by default):

```sh
./configure --enable-trace
```

Binary:

```sh
./build/posish
```

## Run Tests

```sh
make test-smoke
make test-posix
make test-stop
make test-signal
make test-regressions
make rebaseline-fulltruth
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
make test-posix YASH_RUNNER=/absolute/path/to/yash
```

Append pass-rate metrics to `tmp/metrics/posix.csv`:

```sh
make metrics
```

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
- imported vendor test implementation files:
  - `src/builtins/netbsd_test.c`
  - `src/builtins/netbsd_test_vendor.c`

When built with `--enable-trace`, runtime tracing is controlled by:

```sh
POSISH_TRACE=signals,jobs,traps
```

## License

- Shell implementation in this repository: 0BSD (`LICENSE`).
- Vendored third-party test files under `tests/posix/` keep their original
  upstream licenses. See `THIRD_PARTY_NOTICES.md`.
