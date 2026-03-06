# startup-posish.tst: local startup-policy tests for posish

test_o -e 0 'interactive shell on tty loads ENV and ~/.posishrc' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        printf "exit\n" | ../ptyfeed "$TESTEE"
        printf "\n"
    '
__IN__
ENV
RC
~ $ 
__OUT__

test_oE -e 0 'forced interactive shell loads ENV and ~/.posishrc without tty' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        printf "echo BODY\nexit\n" | "$TESTEE" -i
    '
__IN__
ENV
RC
BODY
__OUT__

test_oE -e 0 'command mode skips ENV and ~/.posishrc' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        "$TESTEE" -c "echo BODY"
    '
__IN__
BODY
__OUT__

test_oE -e 0 'script file skips ENV and ~/.posishrc' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        cat >|script.sh <<\__EOF__
echo BODY
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        "$TESTEE" ./script.sh
    '
__IN__
BODY
__OUT__

test_oE -e 0 'login non-interactive shell loads only ~/.posish_profile' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.posish_profile" <<\__EOF__
printf "PROFILE\n"
__EOF__
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        ../loginexec "$TESTEE" -c "echo BODY"
    '
__IN__
PROFILE
BODY
__OUT__

test_o -e 0 'interactive login shell loads profile then ENV then rc' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.posish_profile" <<\__EOF__
printf "PROFILE\n"
__EOF__
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        printf "exit\n" | ../ptyfeed ../loginexec "$TESTEE"
        printf "\n"
    '
__IN__
PROFILE
ENV
RC
~ $ 
__OUT__

test_oE -e 0 'version mode loads no startup files' \
    -c '
        HOME=$PWD
        export HOME
        cat >|"$HOME/.posish_profile" <<\__EOF__
printf "PROFILE\n"
__EOF__
        cat >|"$HOME/.envrc" <<\__EOF__
printf "ENV\n"
__EOF__
        cat >|"$HOME/.posishrc" <<\__EOF__
printf "RC\n"
__EOF__
        ENV='\''$HOME/.envrc'\''
        export ENV
        out=$("$TESTEE" --version)
        case $out in
            (*PROFILE*|*ENV*|*RC*) echo BAD ;;
            (*) echo OK ;;
        esac
    '
__IN__
OK
__OUT__

(
HOME=$PWD
export HOME
cat >|"$HOME/.posish_profile" <<\__EOF__
printf "PROFILE\n"
__EOF__
chmod a-r "$HOME/.posish_profile"

if { <"$HOME/.posish_profile"; } 2>/dev/null; then
    skip=true
fi

test_o -d -e 0 'unreadable login profile is diagnosed and shell continues' \
    -c '
        HOME=$PWD
        export HOME
        ../loginexec "$TESTEE" -c "echo BODY"
    '
__IN__
BODY
__OUT__
)

test_o -e 0 'unset HOME skips ~/.posishrc silently' \
    -c '
        cat >|.posishrc <<\__EOF__
printf "RC\n"
__EOF__
        unset HOME ENV
        PS1="PROMPT "
        export PS1
        printf "exit\n" | ../ptyfeed "$TESTEE"
        printf "\n"
    '
__IN__
PROMPT 
__OUT__

# vim: set ft=sh ts=8 sts=4 sw=4 et:
