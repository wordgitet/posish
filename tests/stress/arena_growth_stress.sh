#!/bin/sh
# SPDX-License-Identifier: 0BSD

set -eu

TESTEE="${TESTEE:-./build/posish}"
MAX_RSS_KB="${MAX_RSS_KB:-262144}"
MAX_GROWTH_FACTOR_X100="${MAX_GROWTH_FACTOR_X100:-130}"
ITERATIONS="${ITERATIONS:-6000}"

if [ ! -x "$TESTEE" ]; then
  echo "arena_growth_stress: TESTEE not executable: $TESTEE" >&2
  exit 2
fi

tmp_script="$(mktemp)"
time1="$(mktemp)"
time2="$(mktemp)"
out1="$(mktemp)"
out2="$(mktemp)"
cleanup() {
  rm -f "$tmp_script" "$time1" "$time2" "$out1" "$out2"
}
trap cleanup EXIT INT TERM

cat >"$tmp_script" <<'EOF'
i=0
while [ "$i" -lt __ITERATIONS__ ]; do
  v$i=$i
  : "${v$i:-x}" >/dev/null
  i=$((i + 1))
done
echo done
EOF

sed -i "s/__ITERATIONS__/$ITERATIONS/g" "$tmp_script"

run_once() {
  out_file="$1"
  time_file="$2"
  /usr/bin/time -f 'elapsed=%E maxrss=%MKB' "$TESTEE" "$tmp_script" >"$out_file" 2>"$time_file"
  if ! grep -qx 'done' "$out_file"; then
    echo "arena_growth_stress: unexpected output" >&2
    cat "$out_file" >&2
    return 1
  fi
}

read_rss() {
  sed -n 's/.*maxrss=\([0-9][0-9]*\)KB.*/\1/p' "$1"
}

run_once "$out1" "$time1"
run_once "$out2" "$time2"

rss1="$(read_rss "$time1")"
rss2="$(read_rss "$time2")"

if [ -z "$rss1" ] || [ -z "$rss2" ]; then
  echo "arena_growth_stress: failed to parse maxrss" >&2
  cat "$time1" >&2
  cat "$time2" >&2
  exit 1
fi

if [ "$rss1" -gt "$MAX_RSS_KB" ] || [ "$rss2" -gt "$MAX_RSS_KB" ]; then
  echo "arena_growth_stress: RSS threshold exceeded ($rss1/$rss2 KB > $MAX_RSS_KB KB)" >&2
  exit 1
fi

limit2=$((rss1 * MAX_GROWTH_FACTOR_X100 / 100))
if [ "$rss2" -gt "$limit2" ]; then
  echo "arena_growth_stress: second run RSS growth too high ($rss1 -> $rss2 KB)" >&2
  exit 1
fi

cat "$time1"
cat "$time2"
echo "arena_growth_stress: PASS"
