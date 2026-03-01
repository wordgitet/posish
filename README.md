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
```

Signal-specific notes:
- `test-signal` uses `TESTCASE_TIMEOUT_SIGNAL=0` by default to avoid
  per-testcase timeout distortion of signal semantics.
- `test-signal-contained` adds an outer file timeout and always reports
  `PASS/FAIL/TIMEOUT` per signal test file.

```sh
make test-signal-contained
```

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

When built with `--enable-trace`, runtime tracing is controlled by:

```sh
POSISH_TRACE=signals,jobs,traps
```

## License

- Shell implementation in this repository: 0BSD (`LICENSE`).
- Vendored third-party test files under `tests/posix/` keep their original
  upstream licenses. See `THIRD_PARTY_NOTICES.md`.
