#!/bin/sh

set -eu

usage() {
    echo "usage: $0 --tests-dir DIR --testee PATH --yash-runner PATH --test-sources \"a.tst b.tst\" [options]" >&2
    echo "options:" >&2
    echo "  --testcase-timeout SEC   per-testcase timeout passed to harness (default: 0)" >&2
    echo "  --file-timeout SEC       outer timeout per test file (default: 180)" >&2
    echo "  --kill-after SEC         timeout -k grace period seconds (default: 5)" >&2
    echo "  --summary PATH           summary output file (default: summary-signal-contained.log)" >&2
    exit 2
}

tests_dir=
testee=
yash_runner=
test_sources=
testcase_timeout=0
file_timeout=180
kill_after=5
summary=summary-signal-contained.log

while [ "$#" -gt 0 ]; do
    case "$1" in
        --tests-dir)
            tests_dir="${2-}"
            shift 2
            ;;
        --testee)
            testee="${2-}"
            shift 2
            ;;
        --yash-runner)
            yash_runner="${2-}"
            shift 2
            ;;
        --test-sources)
            test_sources="${2-}"
            shift 2
            ;;
        --testcase-timeout)
            testcase_timeout="${2-}"
            shift 2
            ;;
        --file-timeout)
            file_timeout="${2-}"
            shift 2
            ;;
        --kill-after)
            kill_after="${2-}"
            shift 2
            ;;
        --summary)
            summary="${2-}"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -n "$tests_dir" ] || usage
[ -n "$testee" ] || usage
[ -n "$yash_runner" ] || usage
[ -n "$test_sources" ] || usage

cd "$tests_dir"

: >"$summary"
printf 'Signal Contained Run\n' >>"$summary"
printf 'testcase_timeout=%s file_timeout=%s kill_after=%s\n' \
    "$testcase_timeout" "$file_timeout" "$kill_after" >>"$summary"

pass_count=0
fail_count=0
timeout_count=0
missing_count=0
total_ok=0
total_err=0
total_skip=0

for tst in $test_sources; do
    trs=${tst%.tst}.trs

    printf '\n>>> %s\n' "$tst" | tee -a "$summary"
    set +e
    timeout -k "${kill_after}s" "${file_timeout}s" \
        make -B "$trs" \
            TESTEE="$testee" \
            YASH_RUNNER="$yash_runner" \
            TESTCASE_TIMEOUT="$testcase_timeout" >/tmp/posish-signal-contained.log 2>&1
    rc=$?
    set -e

    if [ "$rc" -eq 124 ]; then
        timeout_count=$((timeout_count + 1))
        printf 'TIMEOUT rc=%s\n' "$rc" | tee -a "$summary"
        # Clean potentially orphaned workers if timeout hit a PTY wrapper chain.
        pkill -f "run-test.sh $testee $tst" >/dev/null 2>&1 || :
        continue
    fi

    if [ ! -f "$trs" ]; then
        missing_count=$((missing_count + 1))
        printf 'MISSING result file rc=%s\n' "$rc" | tee -a "$summary"
        continue
    fi

    tmp_summary=.signal-contained-summary.$$.tmp
    ./summarize.sh "$trs" >"$tmp_summary"
    ok=$(awk '/^OK:/{print $2}' "$tmp_summary" | tail -n 1)
    err=$(awk '/^ERROR:/{print $2}' "$tmp_summary" | tail -n 1)
    skip=$(awk '/^SKIPPED:/{print $2}' "$tmp_summary" | tail -n 1)
    rm -f "$tmp_summary"
    ok=${ok:-0}
    err=${err:-0}
    skip=${skip:-0}

    total_ok=$((total_ok + ok))
    total_err=$((total_err + err))
    total_skip=$((total_skip + skip))

    if [ "$rc" -ne 0 ] || [ "$err" -ne 0 ]; then
        fail_count=$((fail_count + 1))
        printf 'FAIL rc=%s ok=%s err=%s skip=%s\n' "$rc" "$ok" "$err" "$skip" | tee -a "$summary"
    else
        pass_count=$((pass_count + 1))
        printf 'PASS rc=%s ok=%s err=%s skip=%s\n' "$rc" "$ok" "$err" "$skip" | tee -a "$summary"
    fi
done

{
    printf '\n=== Summary ===\n'
    printf 'PASS: %s\n' "$pass_count"
    printf 'FAIL: %s\n' "$fail_count"
    printf 'TIMEOUT: %s\n' "$timeout_count"
    printf 'MISSING: %s\n' "$missing_count"
    printf 'OK: %s\n' "$total_ok"
    printf 'ERROR: %s\n' "$total_err"
    printf 'SKIPPED: %s\n' "$total_skip"
} | tee -a "$summary"

if [ "$fail_count" -ne 0 ] || [ "$timeout_count" -ne 0 ] || [ "$missing_count" -ne 0 ]; then
    exit 1
fi
