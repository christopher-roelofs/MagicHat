#include "host/console.h"

#include <stdio.h>
#include <string.h>

/*
 * Putting the terminal into raw mode is POSIX. On other hosts the scripted
 * side of --input still works; only the interactive stdin bridge is absent,
 * and --gui is the better way in there anyway.
 */
#if defined(__unix__) || defined(__APPLE__) || defined(__linux__)
#  define MRC_CONSOLE_POSIX 1
#  include <termios.h>
#  include <unistd.h>
#  include <fcntl.h>
#endif

#ifdef MRC_CONSOLE_POSIX
static struct termios saved_termios;
#endif

void mrc_console_init(console *c, bool read_stdin, const char *scripted,
                      uint64_t start_after)
{
    memset(c, 0, sizeof(*c));
    c->scripted   = scripted;
    c->read_stdin = read_stdin;
    c->start_after = start_after;

#ifdef MRC_CONSOLE_POSIX
    if (!read_stdin || !isatty(STDIN_FILENO))
        return;

    /*
     * The monitor is a line-oriented terminal program that does its own
     * echoing, so the host terminal must not cook or echo the input.
     */
    if (tcgetattr(STDIN_FILENO, &saved_termios) == 0) {
        struct termios raw = saved_termios;
        raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) == 0)
            c->raw_stdin = true;
    }
    int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
    if (fl != -1)
        fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
#else
    (void)read_stdin;
    c->read_stdin = false;
#endif
}

void mrc_console_shutdown(console *c)
{
#ifdef MRC_CONSOLE_POSIX
    if (c->raw_stdin) {
        tcsetattr(STDIN_FILENO, TCSANOW, &saved_termios);
        c->raw_stdin = false;
    }
#else
    (void)c;
#endif
}

bool mrc_console_poll(console *c, uint64_t cycle, uint8_t *byte)
{
    if (cycle < c->start_after)
        return false;

    if (c->scripted && c->scripted[c->scripted_pos]) {
        *byte = (uint8_t)c->scripted[c->scripted_pos++];
        return true;
    }

    if (!c->read_stdin)
        return false;

#ifdef MRC_CONSOLE_POSIX
    unsigned char b;
    if (read(STDIN_FILENO, &b, 1) == 1) {
        *byte = b;
        return true;
    }
#endif
    return false;
}
