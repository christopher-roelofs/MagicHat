#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600
#include "host/serial.h"
#include "host/pclink.h"

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <termios.h>
#endif
#include <unistd.h>

#ifdef _WIN32
/*
 * Windows has no pseudo-terminals. A named pipe or a com0com pair could
 * stand in; until then only the in-process link is available here.
 */
bool mh_serial_open_pty(mh_serial *s)
{
    *s = (mh_serial){ .fd = -1 };
    fprintf(stderr, "serial: a pty port is not available on Windows\n");
    return false;
}
#else
bool mh_serial_open_pty(mh_serial *s)
{
    memset(s, 0, sizeof(*s));
    s->fd = posix_openpt(O_RDWR | O_NOCTTY);
    if (s->fd < 0 || grantpt(s->fd) || unlockpt(s->fd)) {
        fprintf(stderr, "serial: cannot open a pty\n");
        if (s->fd >= 0) close(s->fd);
        s->fd = -1;
        return false;
    }

    const char *name = ptsname(s->fd);
    if (!name) {
        close(s->fd);
        s->fd = -1;
        return false;
    }
    snprintf(s->path, sizeof(s->path), "%s", name);

    /*
     * Raw: no echo, no line discipline, no CR/LF translation. This carries a
     * binary protocol, and a terminal helpfully rewriting bytes would corrupt
     * it in ways that look like a flaky cable.
     */
    struct termios t;
    if (tcgetattr(s->fd, &t) == 0) {
        cfmakeraw(&t);
        tcsetattr(s->fd, TCSANOW, &t);
    }
    fcntl(s->fd, F_SETFL, O_NONBLOCK);

    fprintf(stderr, "serial: UART available at %s\n", s->path);
    return true;
}
#endif

void mh_serial_close(mh_serial *s)
{
    if (s->fd >= 0)
        close(s->fd);
    s->fd = -1;
}

void mh_serial_attach(mh_serial *s, mh_pclink *peer)
{
    *s = (mh_serial){ .fd = -1, .peer = peer };
}

bool mh_serial_read(mh_serial *s, uint8_t *byte)
{
    if (s->peer) return mh_pclink_to_guest(s->peer, byte);
    if (s->fd < 0)
        return false;
    return read(s->fd, byte, 1) == 1;
}

void mh_serial_write(mh_serial *s, uint8_t byte)
{
    if (s->peer) { mh_pclink_from_guest(s->peer, byte); return; }
    if (s->fd < 0)
        return;
    ssize_t n = write(s->fd, &byte, 1);
    (void)n;    /* nothing sensible to do if the other end has gone away */
}
