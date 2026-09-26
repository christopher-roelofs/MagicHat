/*
 * console.h — host terminal <-> UART A bridge.
 *
 * UART A is the DataRover's debug serial port and the IDT monitor's console.
 * Transmitted bytes already go to stdout from the UART model; this supplies
 * the other direction.
 */
#ifndef MH_CONSOLE_H
#define MH_CONSOLE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    bool        raw_stdin;      /* stdin was put into raw mode */
    const char *scripted;       /* bytes to feed before reading stdin */
    size_t      scripted_pos;
    bool        read_stdin;
    uint64_t    start_after;    /* deliver nothing before this cycle */
} console;

void mh_console_init(console *c, bool read_stdin, const char *scripted,
                      uint64_t start_after);
void mh_console_shutdown(console *c);

/* Returns true and sets *byte when there is input to deliver. */
bool mh_console_poll(console *c, uint64_t cycle, uint8_t *byte);

#endif /* MH_CONSOLE_H */
