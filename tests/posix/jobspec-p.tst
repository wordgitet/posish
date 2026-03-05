# jobspec-p.tst: focused jobspec regression tests for posish
../checkfg || skip="true" # %REQUIRETTY%

posix="true"

cat >hold <<\__END__
#!/bin/sh
exec sleep 30
__END__

cat >stopjob <<\__END__
#!/bin/sh
exec sh -c 'kill -s STOP $$; echo resumed'
__END__

chmod a+x hold stopjob
ln -f hold ambig-one
ln -f hold ambig-two

test_oE 'kill %1' -m
./hold &
kill %1
wait $!
kill -l $?
__IN__
TERM
__OUT__

test_oE 'kill %+ and %% resolve current job' -m
./hold &
kill %+
wait $!
kill -l $?
./hold &
kill %%
wait $!
kill -l $?
__IN__
TERM
TERM
__OUT__

test_oE 'kill %- resolves previous job' -m
./hold &
first=$!
./hold &
second=$!
kill %-
wait $first
kill -l $?
kill $second 2>/dev/null || :
wait $second 2>/dev/null || :
__IN__
TERM
__OUT__

test_OE -e 17 'wait %1' -m
sh -c 'exit 17' &
wait %1
__IN__

test_oE 'fg %1' -m
./stopjob
fg %1 >/dev/null
__IN__
resumed
__OUT__

test_OE -e 17 'bg %1' -m
sh -c 'kill -s STOP $$; exit 17'
bg %1 >/dev/null
wait %1
__IN__

test_OE -e 0 'ambiguous prefix jobspec' -m
./ambig-one &
./ambig-two &
kill %./ambig 2>err
grep -q 'ambiguous job' err
kill %1 %2
wait
__IN__

test_OE -e 0 'ambiguous substring jobspec' -m
./ambig-one &
./ambig-two &
kill '%?ambig' 2>err
grep -q 'ambiguous job' err
kill %1 %2
wait
__IN__

test_OE -e 0 'invalid numeric jobspec is no such job' -m
./hold &
kill %9999 2>err
grep -q 'no such job' err
kill %1
wait
__IN__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
