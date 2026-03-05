# pwd-p.tst: logical/physical pwd regressions for posish

posix="true"

test_OE -e 0 'pwd defaults to the logical path while -P resolves symlinks'
rm -rf pwd-real pwd-link
mkdir pwd-real
ln -s pwd-real pwd-link
orig=$PWD
cd pwd-link || exit 1
test "$(pwd)" = "$orig/pwd-link"
test "$(pwd -L)" = "$orig/pwd-link"
test "$(pwd -P)" = "$orig/pwd-real"
__IN__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
