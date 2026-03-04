#!/bin/sh
# SPDX-License-Identifier: 0BSD

set -eu

TESTEE="${TESTEE:-./build/posish}"
MAX_RSS_KB="${MAX_RSS_KB:-393216}"
DOC_COUNT="${DOC_COUNT:-400}"

if [ ! -x "$TESTEE" ]; then
  echo "heredoc_pressure: TESTEE not executable: $TESTEE" >&2
  exit 2
fi

tmp_script="$(mktemp)"
time_log="$(mktemp)"
out_log="$(mktemp)"
cleanup() {
  rm -f "$tmp_script" "$time_log" "$out_log"
}
trap cleanup EXIT INT TERM

cat >"$tmp_script" <<'EOF'
i=0
while [ "$i" -lt __DOC_COUNT__ ]; do
  cat <<'DOC' >/dev/null
aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa
bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb
cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc
dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd
eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee
ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff
DOC
  i=$((i + 1))
done
echo done
EOF

sed -i "s/__DOC_COUNT__/$DOC_COUNT/g" "$tmp_script"

/usr/bin/time -f 'elapsed=%E maxrss=%MKB' "$TESTEE" "$tmp_script" >"$out_log" 2>"$time_log"

if ! grep -qx 'done' "$out_log"; then
  echo "heredoc_pressure: unexpected output" >&2
  cat "$out_log" >&2
  exit 1
fi

rss="$(sed -n 's/.*maxrss=\([0-9][0-9]*\)KB.*/\1/p' "$time_log")"
if [ -z "$rss" ]; then
  echo "heredoc_pressure: failed to parse maxrss" >&2
  cat "$time_log" >&2
  exit 1
fi

if [ "$rss" -gt "$MAX_RSS_KB" ]; then
  echo "heredoc_pressure: RSS threshold exceeded ($rss KB > $MAX_RSS_KB KB)" >&2
  exit 1
fi

cat "$time_log"
echo "heredoc_pressure: PASS"
