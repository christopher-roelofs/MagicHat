/*
 * serial.h — expose an emulated UART as a host serial port.
 *
 * Magic Cap's own package transfer runs over the serial line: WinPcLink on a
 * PC waits for the device, and on the device the user goes to the store room
 * and taps the computer. Rather than work out that protocol from the outside,
 * we can hand the real client a port to talk to and let the two negotiate.
 *
 * A pseudo-terminal is the simplest thing that behaves like a serial port to
 * everything else on the system: Wine will map a COM port onto it, and so
 * will minicom or a Python script.
 */
#ifndef MRC_SERIAL_H
#define MRC_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The far end of the wire, which is either a pseudo-terminal for another
 * program to open or the emulator's own package link a few hundred lines
 * away. Both are just bytes in and bytes out, so the UART never learns which
 * it is talking to.
 */
typedef struct mrc_pclink mrc_pclink;

typedef struct {
    int   fd;           /* master side; -1 when not a pty */
    char  path[128];    /* the slave device to point other programs at */
    mrc_pclink *peer;   /* in-process instead; NULL when a pty */
} mrc_serial;

bool mrc_serial_open_pty(mrc_serial *s);
/* Attach the emulator's own link instead of a port. */
void mrc_serial_attach(mrc_serial *s, mrc_pclink *peer);
void mrc_serial_close(mrc_serial *s);

/* Non-blocking: true when a byte was waiting. */
bool mrc_serial_read(mrc_serial *s, uint8_t *byte);
void mrc_serial_write(mrc_serial *s, uint8_t byte);

#endif /* MRC_SERIAL_H */
