# jobpipe-p.tst: stopped pipeline job-control regressions for posish
../checkfg || skip="true" # %REQUIRETTY%

posix="true"

test_OE -e 17 'fg waits for whole stopped pipeline' -m
rm -f marker
exec sh -c 'kill -s STOP $$; printf "x\n"' |
exec sh -c 'kill -s STOP $$; IFS= read -r line; sleep 1; printf "%s\n" "$line" > marker; exit 17'
fg %1 >/dev/null
status=$?
grep -qx 'x' marker
exit $status
__IN__

test_OE -e 17 'bg resumes stopped pipeline and wait returns rightmost status' -m
rm -f marker
exec sh -c 'kill -s STOP $$; printf "x\n"' |
exec sh -c 'kill -s STOP $$; IFS= read -r line; sleep 1; printf "%s\n" "$line" > marker; exit 17'
bg %1 >/dev/null
wait %1
status=$?
grep -qx 'x' marker
exit $status
__IN__

test_oE 'kill %1 signals the whole stopped pipeline group' -m
exec sh -c 'kill -s STOP $$; printf "x\n"' |
exec sh -c 'kill -s STOP $$; IFS= read -r line; sleep 1; printf "%s\n" "$line" > marker; exit 17'
kill %1
kill -s CONT %1
wait %1
kill -l $?
__IN__
TERM
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
