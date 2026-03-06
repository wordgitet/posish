# ast-posish.tst: local AST migration checks for posish

test_o 'sequence executes in order'
echo one; echo two
__IN__
one
two
__OUT__

test_o 'and-or list keeps operator semantics across blank lines'
echo 1 &&
    echo 2 &&

    echo 3
false ||
    false ||

    echo foo
__IN__
1
2
3
foo
__OUT__

test_o 'pipeline keeps blank-line continuation after |'
printf '%s\n' foo bar |
tail -n 1 |

cat
__IN__
bar
__OUT__

test_o 'async list still updates dollar bang'
sleep 0 &
wait $!
printf 'done:%s\n' "$?"
__IN__
done:0
__OUT__

test_o 'compound command nodes still bridge through compatibility path'
if true; then echo ok; fi
for x in 1 2; do echo "$x"; done
__IN__
ok
1
2
__OUT__

test_o 'subshell and brace groups can run list bodies through AST'
(echo one; echo two)
{ echo three; echo four; }
__IN__
one
two
three
four
__OUT__

test_o 'if and while forms can run supported bodies through AST'
if true; then echo yes; else echo no; fi
i=0
while [ "$i" -lt 2 ]; do
    echo "loop:$i"
    i=$((i + 1))
done
until [ "$i" -ge 3 ]; do
    echo "until:$i"
    i=$((i + 1))
done
__IN__
yes
loop:0
loop:1
until:2
__OUT__

test_o 'for loop can run supported body through AST'
for x in a b; do
    echo "for:$x"
done
set -- c d
for y; do
    echo "for:$y"
done
__IN__
for:a
for:b
for:c
for:d
__OUT__

test_o 'function definition inside command substitution still falls back safely'
result=$(f() { echo ok; }; f)
printf '%s\n' "$result"
__IN__
ok
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
