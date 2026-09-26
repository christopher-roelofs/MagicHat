# 68k networking route assessment

This records the ROM, SDK and package inventory for the 68k machines and
the bring-up work done so far. It is not a working system-wide Internet
implementation. The DataRover's working NE2000 path is described in
[NETWORKING.md](NETWORKING.md); do not extrapolate its hardware findings
to these machines, or the reverse.

## Result

Ethernet and modem PC Cards are both candidates, but neither is a verified
drop-in route for the 68k machines. The best-evidenced first route is the
ROM's existing serial-modem software path, followed by PPP to a host-side
virtual ISP; it may not need a card at all. That differs from the
DataRover, whose documented modem is internal and whose UART A is not a
modem port (see [NETWORKING.md](NETWORKING.md), "Dial-up and PPP").

| Target | Evidence | Unverified |
| --- | --- | --- |
| DataRover/MIPS | Working NE2000 PC Card, WCPack/Ne2000 packages, libslirp Internet path | Modem-card and internal-modem Internet paths |
| PIC-2000 | ROM includes PPPClient/PPPServer and TCPStream; Phone uses Hayes on DUART A; optional PPP/libslirp endpoint implemented | Internet service setup/client path, guest LCP/IPCP session, authentication, compatible browser |
| Envoy 1.0 | ROM names include RockwellModem, ExternalModem, PPPClient/PPPServer and GMTPOverPPPLink | Which modem instance is selected, generic IP without added packages, power handling |
| HIX-300 | ROM names include RockwellModem, ExternalModem, PPPClient/PPPServer and GMTPOverPPPLink | Board-specific serial/power wiring, compatible Internet packages |

Names are discovery evidence, not proof that a class is active or that its
methods work. The inventory sampled PIC-2000, Envoy 1.0 and HIX-300; it
does not certify mc31, pt4 or PIC-1000.

## Modem: serial evidence

The Magic Cap 1.5 SDK's `DefFiles/Modem.Def` documents:

- `RockwellModem` under `ASTRO_ROCKWELL`: connected to the processor's
  serial port A, with its stream targeted there.
- `ExternalModem` under `EXTERNAL_MODEM`: a Hayes-compatible modem whose
  reads and writes target `iSerialAServer`.
- `SoftwareModem` as a separate conditional implementation, with its own
  read/write, carrier, command and fax methods.

So on the 68k machines, tracing the installed Rockwell/ExternalModem
instance on MC68349 DUART channel A is better evidenced than assuming a
generic 16550 PC Card. The presence of PCCard, CardServer and HayesPhone
does not prove a generic modem-card driver.

### Channel-A bring-up

`--serial a` exposes the DUART's channel A as a host PTY, independently of
PC Link on channel B. It is opt-in and experimental. Register behavior
follows MC68349 User's Manual section 8: separate A receive/transmit state,
a three-entry receive FIFO, RxRDY/FIFO-full interrupt selection (MR1A bit
6), ISR/IER bits 1/0, reset/enable commands, and the guest-selected baud
fields. Module stop suspends activity. An attached host endpoint asserts
CTSA; CTS changes, modem power, DCD, RTS, breaks, framing errors and exact
shift-register timing are not fully modeled. The PTY retains excess receive
bytes instead of modeling overruns. `MH_68K_BAUD` overrides the baud rate
numerically, and `MH_DUART_TRACE=1` logs DUART register traffic.

The board scheduler services A even without a PC Link peer. Opt-in A state
uses snapshot version 4, carrying FIFO bytes, enables, pending transmission
and deadlines but no host descriptors. Ordinary launches keep writing
version 2 or 3. A restored A session needs `--serial a` again to open a new
host endpoint.

```sh
./build/mhat --rom roms/PIC-2000.rom --no-host-battery \
  --load-state storeroom.state --temporary --serial a
# In another terminal, use the PTY path printed above:
scripts/modem /dev/pts/N
```

PIC-2000's Phone dial action writes CRA `80`, `80`, then `05`. With the
probe attached, the guest proceeds through A interrupts and real
bidirectional Hayes traffic. Captured commands, each answered with `OK`:

```text
ATE0V0
ATS11=100H0\N0&Q6N1S37=0&C1&D2&K3%E0
ATW2L2M1X4S7=30+FCLASS=0
ATL1M1X3H1
```

The following `ATDT1 ;` from the Phone app is a voice dial, **not PPP**,
and the probe rejects it rather than claiming a data carrier. Its parser
does not mistake `&D2` for ATD or `%E0` for echo control. No real telephone
connection is made by `scripts/modem`.

### PPP/libslirp endpoint

PIC-2000 launches can select `--net user` instead of `--serial a`. This
attaches a virtual Hayes modem to channel A and backs asynchronous PPP with
the host libslirp NAT. It answers basic AT initialization, enters data mode
for `ATD` without a voice-dial semicolon, negotiates basic LCP/IPCP without
peer authentication, and passes IPv4 through an Ethernet shim to libslirp.
Defaults are guest `10.0.2.15`, peer `10.0.2.2`, DNS `10.0.2.3`.
`--net-pcap FILE` captures the shim's Ethernet packets and `MH_PPP_TRACE=1`
logs control negotiation and packet direction.

The endpoint has no PAP/CHAP, no LCP/IPCP retransmission timer, no address
pool, and no carrier or power line model. It needs libslirp in the build.
Active PPP sessions are not saved in machine states; reopen the backend
after restoring with `--net user`.

**No guest-originated LCP/IPCP or IP packet has been observed yet.** The
Phone app's `services` panel is for optional telephone services such as
voice mail and exposes no PPP account setup. There is no known guest
action that starts the ROM's PPP client, and no complete 68k
Presto!Links/Presto!Mail package is available. The missing piece is a
compatible 68k Internet client.

Asset-free tests cover bidirectional PTY bytes, baud deadlines,
independent A/B receive interrupts, FIFO threshold and reset behavior,
module stop, CLI selection, and version-4 snapshot round-trip, corruption
and truncation. Real modem traffic is verified only on PIC-2000.

Do not diagnose a package from exception counts alone: ordinary PIC-2000
operation raises address and privilege exceptions by design (odd-address
dispatch through the trampoline at `00000284`, and user-mode time reads;
see [PIC2000.md](PIC2000.md)). `81000381` and `MOVE SR` privilege traces
are not themselves a crash.

## PC Link package installation

`--install PATH` offers a package over the guest's PC Link on DUART channel
B; headless, it stops after the transfer and one second of guest settling.
`MH_PCLINK_TRACE=1` logs the exchange. The DataRover's PC Link handshake
and teardown findings in [NETWORKING.md](NETWORKING.md) apply to the shared
host implementation in `src/host/pclink.c`. PIC-2000 carries GMTP in
UDP/IPv4 inside PPP; HIX-300 uses a different transmit envelope and send
window, described in [HIX300.md](HIX300.md).

The installer accepts version-0 `MCap` distribution envelopes, validates
the name boundary and embedded version-1 `MPkg` header, and transmits the
embedded stream unchanged. The envelope's offset-8 word is not its on-disk
byte count; neither that word nor the embedded serialization is rewritten.
Native tests cover raw CLUS/SALTCOD/MPkg compatibility, malformed and
truncated envelopes, unchanged source bytes, and byte-exact GMTP transfer
without double-wrapping.

Presto!PPP 1.106 (`3PrestoPPP.cap`, a 103 KiB MCap/MPkg FrozenPackage)
installs this way on PIC-2000: the guest acknowledged the complete
transfer with no error dialog, and after save/restore the built-in storage
shelf shows `Presto!PPP` (104K), whose package page reads `Presto!PPP`,
version `1.106`, General Magic. The page has no launch action; it is an
installable service/update package (its release note describes CHAP/ISP
fixes for Presto!Mail and Presto!Links over 1.103), not an application.
Its decoded classes include PPPLinkServer, TCPStream, UDP, DNSResolver and
CHAP configuration, but no browser or mail client. This verifies
installation and listing, **not** PPP activation or Internet access.

```sh
MH_PCLINK_TRACE=1 ./build/mhat --rom roms/PIC-2000.rom \
  --headless --temporary --no-host-battery \
  --load-state storeroom.state \
  --install 3PrestoPPP.cap --tap 5000000,1000000,44,157 \
  -n 2000000000 --dump-fb presto.pgm --save-state presto.state
```

From the output state, 50 million slots with a tap at
`1000000,1000000,28,261` reveals the storage shelf; after save/restore, a
tap at `1000000,1000000,280,217` opens the package page.

PPP alone is not proof of general Internet support: the early ROMs also
carry GMTP over PPP for Magic Cap transfers. A usable route needs IP/PPP
negotiation, TCP/IP/DNS as appropriate, and a compatible application.
Presto!Mail product information describes an older Unix-login mail path,
so not every Presto version uses PPP the same way. The DataRover's MIPS
browser is not a 68k browser.

## Ethernet: reusable device, no verified 68k driver

The NE2000 device core and host NAT are reusable, but the DataRover card
wrapper (`src/devices/pccard/ne2000_card.c`) was measured against that
board's controller windows and byte lanes. SDK `CardSlot` definitions
describe separate common, attribute and I/O windows with insertion and
removal events, so this is board work, not a matter of inserting the
existing wrapper.

`Ne2000.pkg` and `EtherLinkIII.pkg` contain MIPS native functions (for
example, Ne2000's function at file offset 10916 starts `27bdffd0 afbc0010
afb20020 00809021`, a MIPS stack-frame sequence). No 68k Ne2000/EtherLink
driver has been found; that is not proof one never existed.

### ROM-declared CardSlot windows

The ROM object index gives the actual slot instances (SDK classes
`CardSlot` 802 and `CardSlotAstro` 1512):

| ROM | Slot | Common | Attribute | I/O | Length |
| --- | ---: | ---: | ---: | ---: | ---: |
| PIC-2000 | 1 | `04000000` | `24000000` | `28000000` | `04000000` |
| PIC-2000 | 2 | `08000000` | `2C000000` | `30000000` | `04000000` |
| Envoy 1.0 / mc31 | 1, 2 | same as PIC-2000 | same | same | same |
| HIX-300 mc19 | 1 | `04000000` | `24000000` | `28000000` | `04000000` |

PIC-2000's two `CardSlotAstro` objects are object numbers 5821 and 5822;
the adjacent two-element list at ROM offset `0x2A87AE` references both.
Envoy uses base `CardSlot` instances with the same fields. HIX's single
base `CardSlot` has a shorter layout, so its fields start eight bytes
earlier. These are **software declarations**, not proof of physical
decode, insertion or IRQ wiring. Without a card, the emulator maps
`0x04000000` as extra RAM and `0x08000000` as an empty test-image region;
the SRAM card implementation ([STORAGE.md](STORAGE.md)) replaces those
windows while a card is seated.

### Card status inputs

The ROM's CardSlot status methods read `dev21+EE`:

| Method | ROM | Bits |
| --- | --- | --- |
| `CardDetectState` | `0E08672E` | `0x0040` slot 1, `0x0080` slot 2 |
| `CardReadyState` | `0E0867BC` | bits 8/9 |
| `CardLockswitchState` | `0E086792` | bits 0/1 |
| `CardBatteryState` | `0E086E6E` | two-bit fields in bits 12..15 |

A guest diagnostic package called `CardDetectState` and
`CardInsertionSwitchState` on both PIC-2000 slot objects. With empty
inputs all flags were zero; raising each `dev21+D1` bit individually left
them zero, ruling out those inputs. `--probe-preset dev21:EE=0040`,
`=0080` and `=00C0` produced slot 1, slot 2 and both, confirming the table
above. (`--probe-preset` applies after `--load-state`.) These are static
presence checks, not insertion interrupts. Seated SRAM reports ready,
unlocked and battery state 3, and those inputs are restored after guest
writes.

Slot status events are on `dev21+BA`: bit `0x1000` is slot 1 and `0x2000`
slot 2, gated by the corresponding `+C2` enable bits and delivered at IPL6.
ROM `0E086EAA` registers the slot-2 handlers; the IPL6 dispatcher at
`0E088E80` walks `B8/BA` masked by `C0/C2`, and its `BA=0x2000` branch
calls the slot-2 handler at `0E086B0A`. Card removal raises the event, and
the guest clears it through its normal pending-register write. Startup
insertion is level-detected and does not invent a boot-time interrupt.

### Experimental NE2000 aperture

`--net ne2000` attaches the NE2000 device and libslirp NAT to PIC-2000 at
an emulator-only test aperture, `0x0C010000`. It also mirrors the
byte-wide registers at the ROM-declared slot-2 I/O base `0x30000000`, and
presents the shared NE2000 CIS/COR in the slot-2 attribute window at
`0x2C000000` with an extra 68k-only Magic Cap tuple (below). While
attached, the host supplies slot-2 presence (`0x0080`) and ready
(`0x0200`) on `dev21+EE`, without altering the register latch or
saved-state layout. Both apertures poll; there is no NIC IRQ. `--net-pcap`
records traffic and `MH_68K_NET_TRACE=1` logs aperture accesses.

This is an **experimental mapping**, not evidence of the board's byte
lanes, COR configuration or CS3 ASIC. It is mutually exclusive with
`--net user`. The NIC and live NAT state are not serialized, so saving
while it is attached is refused: install and save a package without the
NIC, then restore with `--net ne2000`.

### Guest-native NE2000 probe

A purpose-built 68k native test package (C built from the SDK's Counter
scene shell, not part of this repository) drove the aperture directly. Its
guest code initialized the NE2000, completed DHCP
DISCOVER/OFFER/REQUEST/ACK (address `10.0.2.15`, gateway `10.0.2.2`, DNS
`10.0.2.3`, 86400-second lease), ARPed for the DNS server
(`52:55:0a:00:02:03`), sent a 71-byte UDP DNS query for `example.com` and
received a 103-byte reply with two A records. Through the slot-2 build the
host recorded COR `0x60` after the guest's CIS header checks, and an
eight-frame capture showed the full exchange. This verifies guest-native
register I/O, TX, RX and a real IP/UDP exchange through the host NAT, and
the byte attribute/I/O paths at `0x2C000000`/`0x30000000`. It is **not**
PC Card discovery by Magic Cap, and the package is not a network driver:
it has no lease renewal, retransmission or error recovery.

Clang 18's m68k backend miscompiled indexed reads of a local byte array
(60 bytes of `FF`) and two packet-reading loops. Workarounds (`static
const` frames, split functions, an unrolled DMA skip) are guest-compiler
issues, not NE2000 hardware requirements. A receive helper must read the
four-byte ring header in its own remote-DMA transaction and then consume
the full advertised payload; a fixed 64-byte window was enough for ARP but
exhausts remote DMA on larger frames.

### OS card discovery

`--net ne2000 --probe-preset dev21:BA=2000` asserts one slot-2 insertion
edge. It delivered one IPL6, reached `0E086B0A`, and the ROM read the
emulated CIS from `0x2C000000` through its tuple parser (around
`0E07CFF2` and `0E07D830`), traversing all standard tuples correctly. With
only those tuples it displayed "card in slot 2 won't work". Supplying the
ready input did not change that.

The 68k SDK's `PCCard.h` defines a Magic Cap vendor tuple (`0xA0`): 32
bytes holding `GMMC`, version `0x00010001`, card type, cluster offset,
unique ID, modification time and CRC. ROM `0E07D38C` validates magic,
version, offset bound and CRC; ROM `0E0BA188` computes reflected CRC-32
with seed zero and no final XOR. The PIC-only CIS view appends a valid
`IOCD` tuple (CRC `0x6D1F5F83`) before END. The ROM then took its
successful Magic-tuple branch and changed the alert to "The storage card
in slot 2 is either blank or unreadable..." (set-up was not selected).

`PCCard.Def` defines `CardServer.CanHandleCard(slot)` and
`CardInsertion(slot)`. PIC-2000 `0E07DD1A..0E07DD5C` walks the **global**
`iCardServers` list (`0x87110000`, flat indexical 136) from last to first,
calls `CanHandleCard` (operation 3655), and sends `CardInsertion` (2328) to
the first non-nil result. In the IOCD run the only candidate was class
`0xAF` (175, `MemoryServer`), which returned `840029B1` and received
`CardInsertion`: the storage prompt is the fallback memory server winning
dispatch. The per-slot `CardSlot.Servers` list does not affect selection.

A diagnostic package that appended its own class (`0x8002`) to
`iCardServers` became the first `CanHandleCard` receiver on the next
insertion edge; it checked the `NDC/Ethernet` version tuple and
`GMMC/IOCD`, returned itself, and initialized the aperture in
`CardInsertion`. The storage dialog no longer appeared, with zero
undecoded accesses. This proves **server selection and safe card
initialization**, not a working driver. A `NewTransient` server instance
still returned `C4000004`, and sending it `CardInsertion` produced runaway
invalid accesses, so the allocation/lifecycle ABI is unresolved. Register
before asserting an insertion edge.

### Toward the ROM's network stack

The SDK's `iProtoEthernetMeans` (`iPrototypeMeans[1]`) resolves to class
479, `LANMeans`, not an Ethernet CardServer. A card server must provide a
`CommunicationStream` and bind it to a LANMeans/GMTP stack; initializing
NE2000 registers alone cannot make LANMeans transmit.

Base `Means.GMTPProtocolStack` and `LANMeans.GMTPProtocolStack` return
`nil`; telephone and link means return concrete GMTP communication-module
classes. A LAN implementation therefore needs a LAN-specific
`GMTPCommModule`. Its minimum surface is `WritePDU`, `HandlePDU`,
`RecommendedWritePDUSize`, `MaximumWritePDUSize`, `MaximumReadPDUSize`,
`SupportsMeans` and `OnDemand`. A PDU is a linked list of `{next,
data-pointer, length}` records. `GMTPServer.HandlePDU` takes a dispatch
record with the PDU chain at offset 4, byte count at 8, source address at
12, destination at 16 and association ID at 20. The SDK's real
`GMTPOverUDP` hierarchy is `Object`, `PDUServiceInterface`,
`PDUClientInterface`, `GMTPCommModule`, with `Init`, `Finalize` and
`GMTPCommModuleInitialize` hooks.

Probe packages established each layer in turn:

- A package `LANMeans` subclass (16-byte inherited layout) assigned to
  `iProtoEthernetMeans`, plus a 36-byte `CommunicationStream` subclass,
  returned the expected custom class IDs: the means-to-stream path is live.
- Minimal stream methods (`Connect` initializes the NIC, `Write` sends a
  frame, `Read` drains one, `CountReadPending` checks the received bit)
  produced the first 68k guest-stream-to-host frame: 60 bytes, broadcast,
  source `02:00:00:68:00:01`, EtherType `0x88B5`. An ARP request for
  `10.0.2.2` drew libslirp's reply (`52:55:0A:00:02:02`), but the guest's
  `Read` did not see the packet-received status bit although the capture
  contains the reply. Receive consumption is still an open NIC status/ring
  issue.
- A `GMTPCommModule` subclass returned from
  `N2LANMeans.GMTPProtocolStack`, flattening PDUs into
  Ethernet/IPv4/UDP and routing UDP port 15 back to
  `GMTPServer.HandlePDU`, installs and enters its scene, but the ROM then
  falls into a wild-pointer/illegal-instruction loop during GMTP setup
  before any frame is sent. The failure boundary is GMTP module
  registration and initialization.

The Magic Cap 1.5 SDK does not expose an obvious 68k Ethernet driver
interface: `Network.Def` describes MacTCP/Unix UDP streams, `PPP.Def`
defines PPP clients and GMTP-over-PPP, and `TCP.Def` exposes `TCPStream`
with a private socket identifier.

## Next

1. Find or supply a compatible 68k Internet application (likely
   Presto!Links or Presto!Mail) that uses the installed PPP service, then
   observe LCP/IPCP against `--net user` and fix the gaps it reveals.
2. Check that application's activation, dependencies, and whether it uses
   PIC-2000's channel-A modem path.
3. Extend authentication, retries and modem carrier behavior as needed.
4. For Ethernet, compare the ROM's `GMTPOverUDP` initialization and server
   registration with the custom module; success means entering the scene
   without the wild-pointer loop and a nonzero guest frame count.
5. Repeat handshake and IP tests on Envoy and HIX. Do not assume one
   device's wiring works across all ROMs.

The *Using Magic Cap* manual supports general Internet use on DataRover
(printed pages 72, 107 and 170-171); those pages do not establish early
68k ROM compatibility.
