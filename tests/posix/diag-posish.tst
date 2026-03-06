# diag-posish.tst: local diagnostics tests for posish

test_oe -e 2 'stdin syntax error reports line and column' \
    -c '
        printf "%s\n" "echo \"unterminated" | "$TESTEE"
    '
__IN__
__OUT__
posish: <input>:2:1: syntax: unexpected EOF while looking for matching quote
__ERR__

test_oe -e 2 'script file syntax error reports file path line and column' \
    -c '
        cat >|./bad.sh <<\__EOF__
echo ok
echo "unterminated
__EOF__
        "$TESTEE" ./bad.sh
    '
__IN__
ok
__OUT__
posish: ./bad.sh:3:1: syntax: unexpected EOF while looking for matching quote
__ERR__

test_oe -e 2 'builtin option diagnostics keep their wording' \
    -c '
        "$TESTEE" -c "cd -Z"
    '
__IN__
__OUT__
posish: cd: invalid option: -Z
__ERR__

test_oe -e 1 'runtime diagnostics without trusted positions stay non-positional' \
    -c '
        "$TESTEE" -c "echo hi 1>&foo"
    '
__IN__
__OUT__
posish: invalid file descriptor redirection: foo
__ERR__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
