# posish

`posish` is a work-in-progress POSIX shell implementation in C11.

For the command reference, read:

```sh
man ./posish.1
```

## Build

Typical local build:

```sh
autoreconf -fi
./configure
make -j"$(nproc)"
```

If you want tracing support:

```sh
./configure --enable-trace
make -j"$(nproc)"
```

If Autoconf cached the wrong compiler, rebuild with an explicit `CC`:

```sh
make -B all CC=cc
```

The shell binary is:

```sh
./build/posish
```

## Test

The imported POSIX harness runs through `yash`. If `yash` was not found during
configure, pass `YASH_RUNNER` explicitly.

Common targets:

```sh
make test-smoke YASH_RUNNER=/absolute/path/to/yash
make test-regressions YASH_RUNNER=/absolute/path/to/yash
make test-posix-nosignal YASH_RUNNER=/absolute/path/to/yash
make test-stop YASH_RUNNER=/absolute/path/to/yash
make test-signal YASH_RUNNER=/absolute/path/to/yash
make test-posix YASH_RUNNER=/absolute/path/to/yash
```

You can also point the harness at another shell binary:

```sh
make test-posix TESTEE=/absolute/path/to/shell YASH_RUNNER=/absolute/path/to/yash
```

Useful notes:

- `test-signal` and `test-stop` default to truth-mode timing for better signal semantics.
- `test-signal-contained` adds an outer timeout and reports per-file `PASS`, `PARTIAL_FAIL`, `FULL_FAIL`, `TIMEOUT`, or `MISSING`.
- `make metrics` appends pass-rate data to `tmp/metrics/posix.csv`.

## Runtime Notes

Current startup-file behavior is user-scoped only:

- interactive shell: `ENV`, then `~/.posishrc`
- login shell: `~/.posish_profile`
- interactive login shell: `~/.posish_profile`, then `ENV`, then `~/.posishrc`
- non-interactive `-c` and script execution: no startup files
- non-interactive login shell: `~/.posish_profile`

`posish` does not currently load system-wide startup files such as `/etc/profile`.

Interactive defaults:

- `PS1='\w \$ '`
- `PS2='> '`

When tracing is enabled at configure time, runtime tracing is controlled with:

```sh
POSISH_TRACE=signals,jobs,traps
```

## Termux

`posish` builds on Termux with:

```sh
pkg install build-essential autoconf automake
autoreconf -fi
CC=gcc sh ./configure --prefix="$PREFIX"
make -j"$(nproc)"
make install
```

## License

- Shell implementation in this repository: 0BSD (`LICENSE`)
- Vendored third-party tests under `tests/posix/`: see `THIRD_PARTY_NOTICES.md`
