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

# vim: set ft=sh ts=8 sts=4 sw=4 et:
