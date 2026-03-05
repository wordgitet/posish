# POSIX Test Suite (vendored from yash)

This directory contains the POSIX-oriented yash test corpus (`*-p.tst`) and
its harness files, imported for validating `posish`.

Scope notes:
- Included: POSIX tests (`*-p.tst`) and required harness/helpers.
- Excluded: yash-specific tests (`*-y.tst`).

Run all POSIX tests:

```sh
make test-posix TESTEE=/absolute/path/to/shell YASH_RUNNER=/absolute/path/to/yash
```

`yash` is required as the runner for `run-test.sh`. If missing, the make
targets fail with an installation hint. Override with:

```sh
make test-posix TESTEE=/absolute/path/to/shell YASH_RUNNER=/absolute/path/to/yash
```

Run smoke profile:

```sh
make test-smoke TESTEE=/absolute/path/to/shell YASH_RUNNER=/absolute/path/to/yash
```

Run one test file:

```sh
make test-one TEST=alias-p.tst TESTEE=/absolute/path/to/shell YASH_RUNNER=/absolute/path/to/yash
```

Generated outputs (`*.trs`, `summary.log`, temporary directories) are local
artifacts and can be removed with:

```sh
make clean
```

If `make clean` fails because a previous interrupted run left behind
permissioned `tmp.*` artifacts, rebuild without relying on `clean`:

```sh
make -B -C ../.. all CC=cc
```
