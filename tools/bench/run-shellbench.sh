#!/bin/sh

set -eu

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)

if command -v shellbench >/dev/null 2>&1; then
  SHELLBENCH_BIN=$(command -v shellbench)
else
  SHELLBENCH_BIN="$ROOT_DIR/tmp/external/shellbench/shellbench"
fi

if [ ! -x "$SHELLBENCH_BIN" ]; then
  echo "shellbench not found: $SHELLBENCH_BIN" >&2
  exit 1
fi

SHELLS=${SHELLBENCH_SHELLS:-"$ROOT_DIR/build/posish,dash,freebsd-sh,yash"}

if [ "$#" -gt 0 ]; then
  set -- "$@"
else
  set -- \
    "$ROOT_DIR/tmp/external/shellbench/sample/assign.sh" \
    "$ROOT_DIR/tmp/external/shellbench/sample/count.sh" \
    "$ROOT_DIR/tmp/external/shellbench/sample/cmp.sh" \
    "$ROOT_DIR/tmp/external/shellbench/sample/func.sh" \
    "$ROOT_DIR/tmp/external/shellbench/sample/null.sh" \
    "$ROOT_DIR/tmp/external/shellbench/sample/output.sh"
fi

echo "Benchmark command:"
printf '  %s -c -s %s' "$SHELLBENCH_BIN" "$SHELLS"
for sample in "$@"; do
  printf ' %s' "$sample"
done
printf '\n'

exec "$SHELLBENCH_BIN" -c -s "$SHELLS" "$@"
