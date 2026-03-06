# prompt-posish.tst: local prompt-extension tests for posish

test_o -e 0 'non-interactive shell does not initialize prompt defaults' \
    -c 'printf "[%s][%s]\n" "${PS1-unset}" "${PS2-unset}"'
__IN__
[unset][unset]
__OUT__

test_o -e 0 'default prompt uses HOME-shortened cwd and sigil' \
    -c '
        unset PS1 PS2
        HOME=$PWD
        export HOME
        sigil="$"
        [ "$(id -u)" -eq 0 ] && sigil="#"
        printf "exit\n" | ../ptyfeed "$TESTEE" -i
        printf "\n"
    '
__IN__
~ $ 
__OUT__

test_o -e 1 'prompt uses simple params and command counter' \
    -c '
        PS1="[\$?:\\#:\\s] "
        PS2="cont> "
        export PS1 PS2
        printf "false\nexit\n" | ../ptyfeed "$TESTEE" -i
        status=$?
        printf "\n"
        exit "$status"
    '
__IN__
[0:1:posish] [1:2:posish] 
__OUT__

test_o -e 0 'unsupported prompt parameter syntax stays literal and PS2 is honored' \
    -c '
        PS1='\''${x:-y}> '\''
        PS2="cont> "
        export PS1 PS2
        {
            printf "%s\n" "echo \\"
            printf "%s\n" "hi"
            printf "%s\n" "exit"
        } | ../ptyfeed "$TESTEE" -i
        printf "\n"
    '
__IN__
${x:-y}> cont> hi
${x:-y}> 
__OUT__

test_o -e 0 'path and backslash prompt escapes expand predictably' \
    -c '
        PS1="[\\w|\\W|\\\\] "
        unset PS2
        HOME=$PWD
        export PS1 HOME
        printf "exit\n" | ../ptyfeed "$TESTEE" -i
        printf "\n"
    '
__IN__
[~|~|\] 
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
