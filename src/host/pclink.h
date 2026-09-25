/*
 * pclink.h — the PC end of Magic Cap's package link, inside the emulator.
 *
 * Installing software on one of these machines means being the computer it
 * links to. That has worked for a while as a Python script talking to a
 * pseudo-terminal the emulator hands out, which is fine for a workbench and
 * no good at all for a person: it needs a second program, a terminal, and a
 * path typed correctly in both places. On a tablet it is not possible at all.
 *
 * So the same protocol lives here, and the emulator is the computer. Nothing
 * about the guest changes -- it still sends its greeting, still negotiates,
 * still receives the package over its own serial port at its own speed. The
 * only difference is that the far end of the wire is a few hundred lines in
 * the same process rather than a pty and a shell.
 *
 * The transport is deliberately the same shape a serial port has: bytes in,
 * bytes out, no blocking, no threads. The UART pushes a byte in when the
 * guest transmits and pulls one out when it is ready to receive, exactly as
 * it does for a pty, so nothing in the machine knows the difference.
 *
 * MIPS protocol, as reverse-engineered from WinPcLink.exe and the guest ROM and
 * already proven by the script this replaces:
 *
 *   - the guest opens with a four-byte 'ChMa' greeting, once;
 *   - after that the wire carries blocks: a big-endian uint16 length of 1 to
 *     256, that many bytes, then a CRC-32 of them (all ones to start, no
 *     final complement);
 *   - inside the blocks is a byte stream in which 0x0e, 0x0f and 0x10 are
 *     quoted by a preceding 0x10; unescaped 0x0e and 0x0f are end and abort
 *     controls and do not appear in the stream;
 *   - the stream carries commands: a four-byte tag, a big-endian uint32
 *     length, and that many bytes of payload.
 *
 * PIC-2000 carries commands in GMTP messages over UDP/IPv4/async PPP, with
 * checksums, sequence acknowledgments, and one outgoing message in flight.
 * Its SPkg offer contains a byte filename. After Send, SBuf commands carry an
 * MPkg/Wireline FrozenPackage stream and SndX ends it. Compiler CLUS files
 * are wrapped automatically; MCap distribution envelopes are stripped to
 * their existing MPkg streams. A completed transfer still needs guest-side
 * installation and launch verification.
 */
#ifndef MRC_HOST_PCLINK_H
#define MRC_HOST_PCLINK_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mrc_pclink mrc_pclink;

/*
 * How far the link has got. A window shows this; a command line prints it.
 * Both need to be able to say "nothing is happening yet" without it looking
 * like a failure, because the guest has to be walked to its Storeroom and
 * told to link before anything at all happens.
 */
typedef enum {
    MRC_PCLINK_WAITING,    /* the guest has not linked yet */
    MRC_PCLINK_CONNECTED,  /* linked, package offered */
    MRC_PCLINK_SENDING,    /* bytes on the wire */
    MRC_PCLINK_DONE,       /* the guest took it */
    MRC_PCLINK_FAILED,     /* see mrc_pclink_message */
} mrc_pclink_state;

/*
 * Open the link with a package to offer. The file is read now, so it can be
 * moved or deleted immediately afterwards and a failure to read it is
 * reported before any of the guest's time is spent.
 */
mrc_pclink *mrc_pclink_open(const char *package_path);
void mrc_pclink_close(mrc_pclink *link);

/* The wire. `from_guest` takes a transmitted byte; `to_guest` offers the next
 * byte to receive and returns false when there is nothing to send. */
void mrc_pclink_from_guest(mrc_pclink *link, uint8_t byte);
bool mrc_pclink_to_guest(mrc_pclink *link, uint8_t *byte);

mrc_pclink_state mrc_pclink_state_of(const mrc_pclink *link);
/* What happened, in words for a person. Never NULL. */
const char *mrc_pclink_message(const mrc_pclink *link);
/* Progress, for something to draw. Total is the package's own size. */
uint32_t mrc_pclink_sent(const mrc_pclink *link);
uint32_t mrc_pclink_total(const mrc_pclink *link);

#ifdef __cplusplus
}
#endif
#endif /* MRC_HOST_PCLINK_H */
