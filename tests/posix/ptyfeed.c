/* ptyfeed.c: run a command in a pseudo-terminal and feed stdin to it */
/*
MIT License

Copyright (c) 2016-2019 WATANABE Yuki
Copyright (c) 2026 Mario and contributors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#define _XOPEN_SOURCE 600
#define _DARWIN_C_SOURCE 1
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <sys/wait.h>
#include <unistd.h>

static const char *program_name;

static void error_exit(const char *message) {
    fprintf(stderr, "%s: %s\n", program_name, message);
    exit(EXIT_FAILURE);
}

static void errno_exit(const char *message) {
    fprintf(stderr, "%s: ", program_name);
    perror(message);
    exit(EXIT_FAILURE);
}

static int prepare_master_pseudo_terminal(void) {
    int fd;

    fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (fd < 0) {
        errno_exit("cannot open master pseudo-terminal");
    }
    if (fd <= STDERR_FILENO) {
        error_exit("stdin/stdout/stderr are not open");
    }

    if (grantpt(fd) < 0) {
        errno_exit("pseudo-terminal permission not granted");
    }
    if (unlockpt(fd) < 0) {
        errno_exit("pseudo-terminal permission not unlocked");
    }

    return fd;
}

static const char *slave_pseudo_terminal_name(int master_fd) {
    const char *name;

    errno = 0;
    name = ptsname(master_fd);
    if (name == NULL) {
        errno_exit("cannot name slave pseudo-terminal");
    }
    return name;
}

static void become_session_leader(void) {
    if (setsid() < 0) {
        errno_exit("cannot create new session");
    }
}

static void prepare_slave_pseudo_terminal_fds(const char *slave_name) {
    int slave_fd;

    if (close(STDIN_FILENO) < 0) {
        errno_exit("cannot close old stdin");
    }
    slave_fd = open(slave_name, O_RDWR);
    if (slave_fd != STDIN_FILENO) {
        errno_exit("cannot open slave pseudo-terminal at stdin");
    }

    if (close(STDOUT_FILENO) < 0) {
        errno_exit("cannot close old stdout");
    }
    if (dup(slave_fd) != STDOUT_FILENO) {
        errno_exit("cannot open slave pseudo-terminal at stdout");
    }

    if (close(STDERR_FILENO) < 0) {
        errno_exit("cannot close old stderr");
    }
    if (dup(slave_fd) != STDERR_FILENO) {
        errno_exit("cannot open slave pseudo-terminal at stderr");
    }

#ifdef TIOCSCTTY
    ioctl(slave_fd, TIOCSCTTY, NULL);
#endif

    if (tcgetpgrp(slave_fd) != getpgrp()) {
        error_exit("cannot become controlling process of slave pseudo-terminal");
    }
}

static void disable_terminal_echo(int fd) {
    struct termios tio;

    if (tcgetattr(fd, &tio) != 0) {
        return;
    }
    tio.c_lflag &= (tcflag_t)~ECHO;
    (void)tcsetattr(fd, TCSANOW, &tio);
}

static void exec_command(char *argv[]) {
    execvp(argv[0], argv);
    errno_exit(argv[0]);
}

static char *read_all_stdin(size_t *size_out) {
    char *buf;
    size_t len;
    size_t cap;

    buf = NULL;
    len = 0;
    cap = 0;
    for (;;) {
        ssize_t nread;
        char tmp[4096];

        nread = read(STDIN_FILENO, tmp, sizeof(tmp));
        if (nread < 0) {
            free(buf);
            errno_exit("cannot read stdin");
        }
        if (nread == 0) {
            break;
        }
        if (len + (size_t)nread + 1 > cap) {
            char *grown;
            size_t new_cap;

            new_cap = (cap == 0) ? 4096u : cap;
            while (len + (size_t)nread + 1 > new_cap) {
                new_cap *= 2u;
            }
            grown = realloc(buf, new_cap);
            if (grown == NULL) {
                free(buf);
                error_exit("cannot allocate input buffer");
            }
            buf = grown;
            cap = new_cap;
        }
        memcpy(buf + len, tmp, (size_t)nread);
        len += (size_t)nread;
    }

    if (len + 1 > cap) {
        char *grown;

        grown = realloc(buf, len + 1);
        if (grown == NULL) {
            free(buf);
            error_exit("cannot allocate eof marker");
        }
        buf = grown;
    }
    buf[len++] = '\004';
    *size_out = len;
    return buf;
}

static void write_all(int fd, const char *buf, size_t len) {
    size_t written;

    written = 0;
    while (written < len) {
        ssize_t nwrite;

        nwrite = write(fd, buf + written, len - written);
        if (nwrite < 0) {
            if (errno == EINTR) {
                continue;
            }
            errno_exit("cannot write to pseudo-terminal");
        }
        written += (size_t)nwrite;
    }
}

static void write_normalized_output(const char *buf, size_t len) {
    size_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] == '\r' && i + 1 < len && buf[i + 1] == '\n') {
            continue;
        }
        write_all(STDOUT_FILENO, buf + i, 1);
    }
}

static void forward_all_output(int master_fd) {
    for (;;) {
        char buf[BUFSIZ];
        ssize_t nread;

        nread = read(master_fd, buf, sizeof(buf));
        if (nread < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EIO) {
                break;
            }
            errno_exit("cannot read pseudo-terminal output");
        }
        if (nread == 0) {
            break;
        }
        write_normalized_output(buf, (size_t)nread);
    }
}

static int await_child(pid_t child_pid) {
    int wait_status;

    if (waitpid(child_pid, &wait_status, 0) != child_pid) {
        errno_exit("cannot await child process");
    }
    if (WIFEXITED(wait_status)) {
        return WEXITSTATUS(wait_status);
    }
    if (WIFSIGNALED(wait_status)) {
        return WTERMSIG(wait_status) | 0x80;
    }
    return EXIT_FAILURE;
}

int main(int argc, char *argv[]) {
    int master_fd;
    const char *slave_name;
    int slave_fd;
    pid_t child_pid;
    size_t input_size;
    char *input;

    if (argc <= 0) {
        return EXIT_FAILURE;
    }
    program_name = argv[0];
    if (argc <= 1) {
        error_exit("operand missing");
    }

    input = read_all_stdin(&input_size);
    master_fd = prepare_master_pseudo_terminal();
    slave_name = slave_pseudo_terminal_name(master_fd);
    slave_fd = open(slave_name, O_RDWR | O_NOCTTY);
    if (slave_fd < 0) {
        free(input);
        errno_exit("cannot open slave pseudo-terminal");
    }
    disable_terminal_echo(slave_fd);

    child_pid = fork();
    if (child_pid < 0) {
        free(input);
        errno_exit("cannot spawn child process");
    }
    if (child_pid == 0) {
        close(master_fd);
        become_session_leader();
        prepare_slave_pseudo_terminal_fds(slave_name);
        close(slave_fd);
        exec_command(&argv[1]);
    }

    close(slave_fd);
    write_all(master_fd, input, input_size);
    free(input);
    forward_all_output(master_fd);
    close(master_fd);
    return await_child(child_pid);
}

/* vim: set et sw=4 sts=4 tw=79: */
