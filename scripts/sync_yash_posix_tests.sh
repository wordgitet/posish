#!/bin/sh

set -eu

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/.." && pwd)

DEFAULT_REF="77e38846848c7a5b975aa49d14ed142e4c7823fc"
SRC_SPEC="${1:-github:magicant/yash@$DEFAULT_REF}"
DST_ROOT="${2:-$REPO_ROOT/tests/posix}"
SIGLIST_DST="$REPO_ROOT/tests/siglist.h"

TMP_DIR=
cleanup() {
    if [ -n "${TMP_DIR}" ] && [ -d "${TMP_DIR}" ]; then
        rm -rf "${TMP_DIR}"
    fi
}
trap cleanup EXIT INT TERM

resolve_source() {
    case "$SRC_SPEC" in
        github:*)
            if ! command -v curl >/dev/null 2>&1; then
                echo "curl is required for GitHub sync mode" >&2
                exit 1
            fi

            spec=${SRC_SPEC#github:}
            case "$spec" in
                *@*)
                    repo=${spec%@*}
                    ref=${spec#*@}
                    ;;
                *)
                    echo "invalid github source spec: $SRC_SPEC" >&2
                    echo "expected format: github:<owner>/<repo>@<ref>" >&2
                    exit 1
                    ;;
            esac

            TMP_DIR=$(mktemp -d "$REPO_ROOT/tmp.sync.XXXXXX")
            archive="$TMP_DIR/src.tar.gz"
            url="https://codeload.github.com/$repo/tar.gz/$ref"

            echo "Downloading $url"
            curl -fsSL "$url" -o "$archive"
            tar -xzf "$archive" -C "$TMP_DIR"

            SRC_ROOT=$(find "$TMP_DIR" -mindepth 1 -maxdepth 1 -type d | head -n 1)
            SRC_TESTS="$SRC_ROOT/tests"
            ;;
        *)
            if [ ! -d "$SRC_SPEC" ]; then
                echo "source path not found: $SRC_SPEC" >&2
                exit 1
            fi

            if [ -f "$SRC_SPEC/run-test.sh" ] && [ -d "$SRC_SPEC/.." ]; then
                SRC_TESTS=$SRC_SPEC
                SRC_ROOT=$(CDPATH= cd -- "$SRC_SPEC/.." && pwd)
            elif [ -f "$SRC_SPEC/tests/run-test.sh" ]; then
                SRC_ROOT=$SRC_SPEC
                SRC_TESTS="$SRC_SPEC/tests"
            else
                echo "could not identify yash tests source layout at: $SRC_SPEC" >&2
                exit 1
            fi
            ;;
    esac
}

resolve_source

if [ ! -d "$SRC_TESTS" ]; then
    echo "tests directory not found in source: $SRC_TESTS" >&2
    exit 1
fi
if [ ! -f "$SRC_ROOT/siglist.h" ]; then
    echo "siglist.h not found in source root: $SRC_ROOT" >&2
    exit 1
fi

mkdir -p "$DST_ROOT"
mkdir -p "$(dirname "$SIGLIST_DST")"

# Keep only tracked test-harness files and POSIX tests.
cp "$SRC_TESTS/README.md" "$DST_ROOT/README.yash.md"
cp "$SRC_TESTS/POSIX" "$DST_ROOT/POSIX"
cp "$SRC_TESTS/run-test.sh" "$DST_ROOT/run-test.sh"
cp "$SRC_TESTS/summarize.sh" "$DST_ROOT/summarize.sh"
cp "$SRC_TESTS/enqueue.sh" "$DST_ROOT/enqueue.sh"
cp "$SRC_TESTS/signal.sh" "$DST_ROOT/signal.sh"
cp "$SRC_TESTS/checkfg.c" "$DST_ROOT/checkfg.c"
cp "$SRC_TESTS/ptwrap.c" "$DST_ROOT/ptwrap.c"
cp "$SRC_TESTS/resetsig.c" "$DST_ROOT/resetsig.c"
cp "$SRC_TESTS/valgrind.supp" "$DST_ROOT/valgrind.supp"
cp "$SRC_ROOT/siglist.h" "$SIGLIST_DST"

find "$DST_ROOT" -maxdepth 1 -name '*-p.tst' -delete
find "$SRC_TESTS" -maxdepth 1 -name '*-p.tst' -exec cp {} "$DST_ROOT" \;

if find "$DST_ROOT" -maxdepth 1 -name '*-y.tst' | grep -q .; then
    echo "error: yash-specific tests detected in destination" >&2
    exit 1
fi

chmod +x "$DST_ROOT/run-test.sh" "$DST_ROOT/summarize.sh" "$DST_ROOT/enqueue.sh" "$DST_ROOT/signal.sh"

echo "Synced POSIX tests from: $SRC_SPEC"
echo "Resolved source root: $SRC_ROOT"
echo "Destination tests: $DST_ROOT"
echo "Updated helper header: $SIGLIST_DST"
