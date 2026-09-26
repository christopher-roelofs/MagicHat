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
#ifndef MH_SERIAL_H
#define MH_SERIAL_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The far end of the wire, which is either a pseudo-terminal for another
 * program to open or the emulator's own package link a few hundred lines
 * away. Both are just bytes in and bytes out, so the UART never learns which
 * it is talking to.
 */
typedef struct mh_pclink mh_pclink;

typedef struct {
    int   fd;           /* master side; -1 when not a pty */
    char  path[128];    /* the slave device to point other programs at */
    mh_pclink *peer;   /* in-process instead; NULL when a pty */
} mh_serial;

bool mh_serial_open_pty(mh_serial *s);
/* Attach the emulator's own link instead of a port. */
void mh_serial_attach(mh_serial *s, mh_pclink *peer);
void mh_serial_close(mh_serial *s);

/* Non-blocking: true when a byte was waiting. */
bool mh_serial_read(mh_serial *s, uint8_t *byte);
void mh_serial_write(mh_serial *s, uint8_t byte);

#endif /* MH_SERIAL_H */
