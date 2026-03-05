# arithshift-p.tst: invalid arithmetic shift width regressions for posish

posix="true"

test_OE -e 0 'left shift rejects negative width'
(echo $((1 << -1))) >out 2>err
test $? -ne 0
test ! -s out
grep -q 'invalid shift width' err
__IN__

test_OE -e 0 'right shift rejects negative width'
(echo $((1 >> -1))) >out 2>err
test $? -ne 0
test ! -s out
grep -q 'invalid shift width' err
__IN__

test_OE -e 0 'left shift rejects oversized width'
(echo $((1 << 1000))) >out 2>err
test $? -ne 0
test ! -s out
grep -q 'invalid shift width' err
__IN__

test_OE -e 0 'shift assignment rejects oversized width'
x=1
(echo $((x <<= 1000))) >out 2>err
test $? -ne 0
test ! -s out
grep -q 'invalid shift width' err
__IN__

test_oE 'valid shifts still work'
echo $((1 << 1))
echo $((8 >> 1))
__IN__
2
4
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
