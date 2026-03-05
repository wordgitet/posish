# trapcmdsub-p.tst: EXIT trap regressions inside command substitution

posix="true"

test_oE 'EXIT trap runs for command substitution after function return'
result=$(trap 'printf trap; echo ped' EXIT; f() { return; }; f)
printf '%s\n' "$result"
echo --- $?
__IN__
trapped
--- 0
__OUT__

test_oE 'EXIT trap runs for command substitution after explicit exit'
result=$(trap 'printf trap; echo ped' EXIT; exit 7)
status=$?
printf '%s\n' "$result"
echo --- $status
__IN__
trapped
--- 7
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
