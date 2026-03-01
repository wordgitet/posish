#!/bin/sh

set -eu

usage() {
    echo "usage: $0 [--csv PATH] [--checklist PATH]" >&2
    exit 2
}

csv=tests/posix/summary-fulltruth.csv
checklist=tests/POSIX_CHECKLIST.md

while [ "$#" -gt 0 ]; do
    case "$1" in
        --csv)
            csv="${2-}"
            shift 2
            ;;
        --checklist)
            checklist="${2-}"
            shift 2
            ;;
        *)
            usage
            ;;
    esac
done

[ -f "$csv" ] || {
    echo "classification csv not found: $csv" >&2
    exit 1
}
[ -f "$checklist" ] || {
    echo "checklist file not found: $checklist" >&2
    exit 1
}

tmp_output="${checklist}.tmp.$$"

# Strict sync: only FULL_PASS remains checked, everything else is unchecked.
awk -F, -v csv="$csv" '
BEGIN {
    while ((getline line < csv) > 0) {
        gsub(/\r$/, "", line)
        if (line == "test,rc,ok,error,skipped,status") {
            continue
        }
        n = split(line, parts, ",")
        if (n >= 6) {
            status[parts[1]] = parts[6]
        }
    }
    close(csv)
}
{
    if ($0 ~ /^- \[[ x]\] [^[:space:]]+\.tst$/) {
        test_name = $0
        sub(/^- \[[ x]\] /, "", test_name)
        mark = " "
        if (status[test_name] == "FULL_PASS") {
            mark = "x"
        }
        print "- [" mark "] " test_name
        next
    }
    print
}
' "$checklist" >"$tmp_output"

mv "$tmp_output" "$checklist"
echo "Synced checklist: $checklist"
