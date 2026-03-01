#!/bin/sh

set -eu

SUMMARY_FILE="${1:?summary log path is required}"
CSV_FILE="${2:?csv output path is required}"

if [ ! -f "$SUMMARY_FILE" ]; then
    echo "summary log not found: $SUMMARY_FILE" >&2
    exit 1
fi

ok=$(awk '/^OK:/{print $2}' "$SUMMARY_FILE" | tail -n 1)
err=$(awk '/^ERROR:/{print $2}' "$SUMMARY_FILE" | tail -n 1)
skip=$(awk '/^SKIPPED:/{print $2}' "$SUMMARY_FILE" | tail -n 1)

if [ -z "$ok" ] || [ -z "$err" ] || [ -z "$skip" ]; then
    echo "could not parse summary counts from $SUMMARY_FILE" >&2
    exit 1
fi

mkdir -p "$(dirname "$CSV_FILE")"

if [ ! -f "$CSV_FILE" ]; then
    printf 'timestamp,ok,error,skipped,ratio_ok_over_ok_plus_error\n' >"$CSV_FILE"
fi

timestamp=$(date -u '+%Y-%m-%dT%H:%M:%SZ')
ratio=$(awk -v ok="$ok" -v err="$err" 'BEGIN { if ((ok+err)==0) print "0.0000"; else printf "%.4f", ok/(ok+err) }')

printf '%s,%s,%s,%s,%s\n' "$timestamp" "$ok" "$err" "$skip" "$ratio" >>"$CSV_FILE"
printf 'Appended metrics: %s\n' "$CSV_FILE"
