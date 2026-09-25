/*
 * console.h — host terminal <-> UART A bridge.
 *
 * UART A is the DataRover's debug serial port and the IDT monitor's console.
 * Transmitted bytes already go to stdout from the UART model; this supplies
 * the other direction.
 */
#ifndef MRC_CONSOLE_H
#define MRC_CONSOLE_H

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

void mrc_console_init(console *c, bool read_stdin, const char *scripted,
                      uint64_t start_after);
void mrc_console_shutdown(console *c);

/* Returns true and sets *byte when there is input to deliver. */
bool mrc_console_poll(console *c, uint64_t cycle, uint8_t *byte);

#endif /* MRC_CONSOLE_H */
