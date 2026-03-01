#!/bin/sh

set -eu

usage() {
    echo "usage: $0 --tests-dir DIR --testee PATH --yash-runner PATH --test-sources \"a.tst b.tst\" [options]" >&2
    echo "options:" >&2
    echo "  --testcase-timeout SEC   per-testcase timeout passed to harness (default: 0)" >&2
    echo "  --file-timeout SEC       outer timeout per test file (default: 200)" >&2
    echo "  --kill-after SEC         timeout -k grace period seconds (default: 5)" >&2
    echo "  --summary PATH           summary output file (default: summary-fulltruth.log)" >&2
    echo "  --classification-csv PATH classification csv output file (default: summary-fulltruth.csv)" >&2
    exit 2
}

tests_dir=
testee=
yash_runner=
test_sources=
testcase_timeout=0
file_timeout=200
kill_after=5
summary=summary-fulltruth.log
classification_csv=summary-fulltruth.csv

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
        --classification-csv)
            classification_csv="${2-}"
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

full_pass_list="${summary%.log}.full-pass.list"
partial_fail_list="${summary%.log}.partial-fail.list"
full_fail_list="${summary%.log}.full-fail.list"
timeout_list="${summary%.log}.timeout.list"
missing_list="${summary%.log}.missing.list"

: >"$summary"
: >"$classification_csv"
: >"$full_pass_list"
: >"$partial_fail_list"
: >"$full_fail_list"
: >"$timeout_list"
: >"$missing_list"

printf 'test,rc,ok,error,skipped,status\n' >"$classification_csv"
printf 'Full Truth Contained Run\n' >>"$summary"
printf 'testcase_timeout=%s file_timeout=%s kill_after=%s\n' \
    "$testcase_timeout" "$file_timeout" "$kill_after" >>"$summary"

full_pass_count=0
partial_fail_count=0
full_fail_count=0
timeout_count=0
missing_count=0
total_ok=0
total_err=0
total_skip=0

for tst in $test_sources; do
    trs=${tst%.tst}.trs
    rc=0
    ok=0
    err=0
    skip=0
    status=FULL_FAIL

    printf '\n>>> %s\n' "$tst" | tee -a "$summary"
    set +e
    timeout -k "${kill_after}s" "${file_timeout}s" \
        make -B "$trs" \
            TESTEE="$testee" \
            YASH_RUNNER="$yash_runner" \
            TESTCASE_TIMEOUT="$testcase_timeout" >/tmp/posish-fulltruth-contained.log 2>&1
    rc=$?
    set -e

    if [ "$rc" -eq 124 ]; then
        status=TIMEOUT
        timeout_count=$((timeout_count + 1))
        printf '%s\n' "$tst" >>"$timeout_list"
        printf 'TIMEOUT rc=%s\n' "$rc" | tee -a "$summary"
        # Timeout can leave wrapper descendants alive; clean by test tuple.
        pkill -f "run-test.sh $testee $tst" >/dev/null 2>&1 || :
        printf '%s,%s,%s,%s,%s,%s\n' "$tst" "$rc" "$ok" "$err" "$skip" "$status" >>"$classification_csv"
        continue
    fi

    if [ ! -f "$trs" ]; then
        status=MISSING
        missing_count=$((missing_count + 1))
        printf '%s\n' "$tst" >>"$missing_list"
        printf 'MISSING result file rc=%s\n' "$rc" | tee -a "$summary"
        printf '%s,%s,%s,%s,%s,%s\n' "$tst" "$rc" "$ok" "$err" "$skip" "$status" >>"$classification_csv"
        continue
    fi

    tmp_summary=.fulltruth-summary.$$.tmp
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

    # Classification is strict and deterministic to power checklist sync.
    if [ "$rc" -eq 0 ] && [ "$err" -eq 0 ]; then
        status=FULL_PASS
        full_pass_count=$((full_pass_count + 1))
        printf '%s\n' "$tst" >>"$full_pass_list"
    elif [ "$ok" -gt 0 ] && [ "$err" -gt 0 ]; then
        status=PARTIAL_FAIL
        partial_fail_count=$((partial_fail_count + 1))
        printf '%s\n' "$tst" >>"$partial_fail_list"
    else
        status=FULL_FAIL
        full_fail_count=$((full_fail_count + 1))
        printf '%s\n' "$tst" >>"$full_fail_list"
    fi

    printf '%s rc=%s ok=%s err=%s skip=%s\n' "$status" "$rc" "$ok" "$err" "$skip" | tee -a "$summary"
    printf '%s,%s,%s,%s,%s,%s\n' "$tst" "$rc" "$ok" "$err" "$skip" "$status" >>"$classification_csv"
done

ratio=$(awk -v ok="$total_ok" -v err="$total_err" 'BEGIN { if ((ok+err)==0) print "0.0000"; else printf "%.4f", ok/(ok+err) }')

{
    printf '\n=== Summary ===\n'
    printf 'FULL_PASS: %s\n' "$full_pass_count"
    printf 'PARTIAL_FAIL: %s\n' "$partial_fail_count"
    printf 'FULL_FAIL: %s\n' "$full_fail_count"
    printf 'TIMEOUT: %s\n' "$timeout_count"
    printf 'MISSING: %s\n' "$missing_count"
    printf 'OK: %s\n' "$total_ok"
    printf 'ERROR: %s\n' "$total_err"
    printf 'SKIPPED: %s\n' "$total_skip"
    printf 'RATIO_OK_OVER_OK_PLUS_ERROR: %s\n' "$ratio"
} | tee -a "$summary"

# Rebaseline should always finish classification for downstream reporting/sync.
exit 0
