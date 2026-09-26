/*
 * tx39_regs.h — Toshiba TX39 (TMPR3902U) on-chip peripheral register map.
 *
 * Derived from NetBSD sys/arch/hpcmips/tx/ register headers.
 * and cross-checked against MagicCap-USA.image disassembly.
 *
 * The whole block is one 1 KiB window at physical 0x10C00000, reachable as
 * kseg1 0xB0C00000 (uncached) — which is how the Magic Cap ROM addresses it.
 */
#ifndef MH_TX39_REGS_H
#define MH_TX39_REGS_H

#define TX39_CFG_BASE           0x10C00000u
#define TX39_CFG_SIZE           0x00000400u

/* ---- BIU: memory / chip-select configuration ---- 0x000..0x024 */
#define TX39_MEMCONFIG0         0x000
#define TX39_MEMCONFIG1         0x004
#define TX39_MEMCONFIG2         0x008
#define TX39_MEMCONFIG3         0x00C
#define TX39_MEMCONFIG4         0x010
#define TX39_MEMCONFIG5         0x014
#define TX39_MEMCONFIG6         0x018
#define TX39_MEMCONFIG7         0x01C
#define TX39_MEMCONFIG8         0x020

/* ---- Video (LCD controller) ---- 0x028..0x05C */
#define TX39_VIDEOCTRL1         0x028
#define TX39_VIDEOCTRL2         0x02C
#define TX39_VIDEOCTRL3         0x030   /* frame buffer base, plane A */
#define TX39_VIDEOCTRL4         0x034
#define TX39_VIDEOCTRL5         0x038
#define TX39_VIDEOCTRL6         0x03C
#define TX39_VIDEOCTRL7         0x040
#define TX39_VIDEOCTRL8         0x044
#define TX39_VIDEOCTRL9         0x048
#define TX39_VIDEOCTRL10        0x04C
#define TX39_VIDEOCTRL11        0x050
#define TX39_VIDEOCTRL12        0x054
#define TX39_VIDEOCTRL13        0x058
#define TX39_VIDEOCTRL14        0x05C

/* VIDEOCTRL1 bits */
#define VID1_LINECNT_SHIFT      22
#define VID1_LINECNT_MASK       0x3FFu
#define VID1_LOADDLY            0x00200000u
#define VID1_BAUDVAL_SHIFT      16
#define VID1_BAUDVAL_MASK       0x1Fu
#define VID1_VIDDONEVAL_SHIFT   9
#define VID1_VIDDONEVAL_MASK    0x7Fu
#define VID1_ENFREEZEFRAME      0x00000100u
#define VID1_BITSEL_SHIFT       6
#define VID1_BITSEL_MASK        0x3u
#define VID1_BITSEL_MONO        0x0u
#define VID1_BITSEL_2BPP        0x1u
#define VID1_BITSEL_4BPP        0x2u
#define VID1_BITSEL_8BPP        0x3u
#define VID1_DISPSPLIT          0x00000020u
#define VID1_DISP8              0x00000010u
#define VID1_DFMODE             0x00000008u
#define VID1_INVVID             0x00000004u
#define VID1_DISPON             0x00000002u
#define VID1_ENVID              0x00000001u

/* VIDEOCTRL2 fields */
#define VID2_VIDRATE_SHIFT      22
#define VID2_VIDRATE_MASK       0x3FFu
#define VID2_HORZVAL_SHIFT      12
#define VID2_HORZVAL_MASK       0x1FFu
#define VID2_LINEVAL_SHIFT      0
#define VID2_LINEVAL_MASK       0x3FFu

/* VIDEOCTRL3 fields: framebuffer base = (VIDBANK << 20) | (VIDBASEHI << 4) */
#define VID3_VIDBANK_SHIFT      20
#define VID3_VIDBANK_MASK       0xFFFu
#define VID3_VIDBASEHI_SHIFT    4
#define VID3_VIDBASEHI_MASK     0xFFFFu

/* ---- SIB: serial interface bus (host side of the UCB1100 codec) ---- */
#define TX39_SIBSIZE            0x060
#define TX39_SIBSNDRXSTART      0x064
#define TX39_SIBSNDTXSTART      0x068
#define TX39_SIBTELRXSTART      0x06C
#define TX39_SIBTELTXSTART      0x070
#define TX39_SIBCTRL            0x074
#define TX39_SIBSNDHOLD         0x078
#define TX39_SIBTELHOLD         0x07C
#define TX39_SIBSF0CTRL         0x080   /* subframe 0 command  (CPU -> UCB) */
#define TX39_SIBSF1CTRL         0x084
#define TX39_SIBSF0STAT         0x088   /* subframe 0 response (UCB -> CPU) */
#define TX39_SIBSF1STAT         0x08C
#define TX39_SIBDMACTRL         0x090

/* SIB subframe command/status field layout */
#define SIBSF_REGADDR_SHIFT     27
#define SIBSF_REGADDR_MASK      0xFu
#define SIBSF_REGDATA_SHIFT     0
#define SIBSF_REGDATA_MASK      0xFFFFu
#define SIBSF0_WRITE            0x04000000u   /* 1 = write, 0 = read */
#define SIBSF0_SNDVALID         0x00020000u
#define SIBSF0_TELVALID         0x00010000u

/*
 * SIBSIZE holds the two DMA ring sizes; SIBSNDTXSTART and friends hold their
 * base addresses, expressed in the TX39's kuseg alias of DRAM bank 0 rather
 * than as physical addresses. Magic Cap programs 0x403ED638, which is
 * physical 0x003ED638 — just below the framebuffer.
 */
#define SIBSIZE_SND_SHIFT       18
#define SIBSIZE_TEL_SHIFT       2
#define SIBSIZE_MASK            0xFFFu
#define TX39_KUSEG_DRAM_BANK0   0x40000000u

/*
 * SIBDMACTRL. These are NetBSD's names — the register is fully documented in
 * tx39sibreg.h, which I should have read before guessing at it.
 *
 * SNDDMAPTR is the one that matters: software reads it to see how far the
 * play pointer has advanced, and refills the part of the ring that has been
 * consumed. Returning a constant for it means the pointer never appears to
 * move and there is never a reason to refill.
 */
#define SIBDMA_SNDBUFF1TIME     0x80000000u
#define SIBDMA_SNDDMALOOP       0x40000000u
#define SIBDMA_SNDDMAPTR_SHIFT  18
#define SIBDMA_SNDDMAPTR_MASK   0xFFFu
#define SIBDMA_ENDMARXSND       0x00020000u
#define SIBDMA_ENDMATXSND       0x00010000u
#define SIBDMA_TELBUFF1TIME     0x00008000u
#define SIBDMA_TELDMALOOP       0x00004000u
#define SIBDMA_TELDMAPTR_SHIFT  2
#define SIBDMA_TELDMAPTR_MASK   0xFFFu
#define SIBDMA_ENDMARXTEL       0x00000002u
#define SIBDMA_ENDMATXTEL       0x00000001u

/* SIBCTRL fields */
#define SIBCTRL_SIBIRQ          0x80000000u
#define SIBCTRL_SCLKDIV_SHIFT   24
#define SIBCTRL_TELFSDIV_SHIFT  16
#define SIBCTRL_SNDFSDIV_SHIFT  8
#define SIBCTRL_SNDFSDIV_MASK   0x7Fu
#define SIBCTRL_SELTELSF1       0x00000080u
#define SIBCTRL_SELSNDSF1       0x00000040u
#define SIBCTRL_ENTEL           0x00000020u
#define SIBCTRL_ENSND           0x00000010u
#define SIBCTRL_SIBLOOP         0x00000008u
#define SIBCTRL_ENSF1           0x00000004u
#define SIBCTRL_ENSF0           0x00000002u
#define SIBCTRL_ENSIB           0x00000001u

/* ---- IrDA ---- */
#define TX39_IRCTRL1            0x0A0
#define TX39_IRCTRL2            0x0A4
#define TX39_IRTXHOLD           0x0A8

/* ---- UART A / B ---- */
#define TX39_UARTA_BASE         0x0B0
#define TX39_UARTB_BASE         0x0C8
#define TX39_UART_CTRL1         0x000   /* + base */
#define TX39_UART_CTRL2         0x004
#define TX39_UART_DMACTRL1      0x008
#define TX39_UART_DMACTRL2      0x00C
#define TX39_UART_DMACNT        0x010
#define TX39_UART_TXHOLD        0x014   /* write */
#define TX39_UART_RXHOLD        0x014   /* read  */

/* UART CTRL1 bits */
#define UART_CTRL1_UARTON       0x80000000u
#define UART_CTRL1_EMPTY        0x40000000u
#define UART_CTRL1_PRXHOLDFULL  0x20000000u
#define UART_CTRL1_RXHOLDFULL   0x10000000u
#define UART_CTRL1_ENDMARX      0x00008000u
#define UART_CTRL1_ENDMATX      0x00004000u
#define UART_CTRL1_LOOPBACK     0x00000010u
#define UART_CTRL1_BIT7         0x00000008u
#define UART_CTRL1_ENPARITY     0x00000002u
#define UART_CTRL1_TWOSTOP      0x00000020u
#define UART_CTRL1_ENUART       0x00000001u

/*
 * ---- MBUS ----
 *
 * The TX39 interrupt controller documents a full set of MBUS sources, but
 * NetBSD's hpcmips port has no MBUS driver and no register header, because
 * the machines it supports do not wire it up. The DataRover does, and the
 * register block sits at 0x0E0..0x0FC, immediately above UART B.
 *
 * The layout below is read off the ROM's own use of it, chiefly the routine
 * at 0x83C06B08:
 *
 *     poll  0x0E0 until bit 31 clears        (busy)
 *     write 0x0E4 = 0x64080000               (data)
 *     write INTRCLEAR2 = MBUSTXBUFAVAIL
 *     write 0x0E0 = 0x000008A3               (command; starts the transfer)
 *     poll  INTRSTATUS2 until MBUSTXBUFAVAIL (completion)
 *     write 0x0F4 = <16-bit value>
 *
 * The Rosemary SDK Dino.h / Dino.asm.h now identify CTRL2, DMA registers,
 * COMMAND and DATA below. E4 was historically misnamed MBUSDATA here:
 * it is the clock/delay control register, not payload data. See KEYBOARD.md.
 * The current runtime still models an empty bus, not active DMA.
 */
#define TX39_MBUSCTRL           0x0E0
#define TX39_MBUSCTRL2          0x0E4
#define TX39_MBUSDMASTART       0x0E8
#define TX39_MBUSDMALENGTH     0x0EC
#define TX39_MBUSDMACOUNT      0x0F0
#define TX39_MBUSCOMMAND        0x0F4
#define TX39_MBUSPAYLOAD        0x0F8
/* Historical aliases; preserve source compatibility while the controller
 * implementation is expanded. MBUSDATA is NOT the payload register. */
#define TX39_MBUSDATA           TX39_MBUSCTRL2
#define TX39_MBUSREG_E8         0x0E8
#define TX39_MBUSREG_EC         0x0EC
#define TX39_MBUSREG_F0         0x0F0
#define TX39_MBUSREG_F4         0x0F4
#define TX39_MBUSREG_F8         0x0F8
#define TX39_MBUSREG_FC         0x0FC
#define TX39_MBUS_FIRST         0x0E0
#define TX39_MBUS_LAST          0x0FC

#define MBUSCTRL_BUSY           0x80000000u
/* ROM 0x83C28364 samples bit 29; 0x83C2A794 treats high as no accessories.
 * This descriptive name and empty-bus polarity are inferred from the ROM. */
#define MBUSCTRL_INPUT_HIGH     0x20000000u

/* INTRSTATUS2 MBUS sources */
#define INT2_MBUSTXBUFAVAIL     0x00000800u
#define INT2_MBUSTXERR          0x00000400u
#define INT2_MBUSEMPTY          0x00000200u
#define INT2_MBUSRXBUFAVAIL     0x00000100u
#define INT2_MBUSRXERR          0x00000080u
#define INT2_MBUSDET            0x00000040u
#define INT2_MBUS_MASK          0x00000FFCu

/* ---- ICU: interrupt controller ---- */
#define TX39_INTRSTATUS(n)      (0x100u + ((n) - 1u) * 4u)   /* n = 1..6, read  */
#define TX39_INTRCLEAR(n)       (0x100u + ((n) - 1u) * 4u)   /* n = 1..5, write */
#define TX39_INTRENABLE(n)      (0x118u + ((n) - 1u) * 4u)   /* n = 1..6        */
#define TX39_ICU_NBANK          6

/* INTRSTATUS1 */
#define INT1_LCDINT             0x80000000u
#define INT1_DFINT              0x40000000u
/*
 * Sound DMA progress. Real hardware raises these as the play pointer passes
 * the halfway mark and as it wraps, and software refills the half of the ring
 * that has just been consumed. Without them the ring is never refilled and
 * whatever was last in it repeats forever.
 */
#define INT1_SND0_5INT          0x00400000u   /* half consumed */
#define INT1_SND1_0INT          0x00200000u   /* wrapped       */
#define INT1_SNDDMACNTINT       0x00040000u

#define INT1_VALTELPOSINT       0x00001000u
#define INT1_VALTELNEGINT       0x00000800u
#define INT1_SNDININT           0x00000400u
#define INT1_TELININT           0x00000200u
#define INT1_SIBSF0INT          0x00000100u
#define INT1_SIBSF1INT          0x00000080u
#define INT1_SIBIRQPOSINT       0x00000040u
#define INT1_SIBIRQNEGINT       0x00000020u
#define INT1_SIB_MASK           0x000001E0u

/* INTRSTATUS2 */
#define INT2_UARTARXINT         0x80000000u
#define INT2_UARTATXINT         0x04000000u
#define INT2_UARTATXOVERRUNINT  0x02000000u
#define INT2_UARTAEMPTYINT      0x01000000u
#define INT2_UARTBRXINT         0x00200000u
#define INT2_UARTBTXINT         0x00010000u
#define INT2_UARTBTXOVERRUNINT  0x00008000u
#define INT2_UARTBEMPTYINT      0x00004000u
#define INT2_MBUSPOSINT         0x00000008u

/* ---- Timer / RTC ---- */
#define TX39_TIMERRTCHI         0x140
#define TX39_TIMERRTCLO         0x144
#define TX39_TIMERALARMHI       0x148
#define TX39_TIMERALARMLO       0x14C
#define TX39_TIMERCONTROL       0x150
#define TX39_TIMERPERIODIC      0x154

/* INTRSTATUS5 — timer / RTC / GPIO edge sources */
#define INT5_RTCINT             0x80000000u
#define INT5_ALARMINT           0x40000000u
#define INT5_PERINT             0x20000000u
#define INT5_STPTIMERINT        0x10000000u
#define INT5_POSONBUTNINT       0x00800000u
#define INT5_NEGONBUTNINT       0x00400000u
#define INT5_TIMER_MASK         0xE0000000u

/* INTRSTATUS6 — high-priority IRQ bank (drives CPU IP4) */
#define INT6_IRQHIGH            0x80000000u
#define INT6_IRQLOW             0x40000000u

/* Timer clocks */
#define TX39_RTCLOCK_HZ         32768u

/* TIMERCONTROL bits */
#define TIMERCTRL_FREEZEPRE     0x00000080u
#define TIMERCTRL_FREEZERTC     0x00000040u
#define TIMERCTRL_FREEZETIMER   0x00000020u
#define TIMERCTRL_ENPERTIMER    0x00000010u
#define TIMERCTRL_RTCCLR        0x00000008u

/* TIMERPERIODIC fields */
#define TIMERPER_PERCNT_SHIFT   15
#define TIMERPER_PERVAL_SHIFT   0
#define TIMERPER_MASK           0xFFFFu

/* ---- SPI ---- */
#define TX39_SPICTRL            0x160
#define TX39_SPIHOLD            0x164

/* ---- MFIO / GPIO ---- */
#define TX39_IOCTRL             0x180
#define IOCTRL_IODIN_MASK       0x0000007fu
#define TX39_IOMFIODATAOUT      0x184
#define TX39_IOMFIODATADIR      0x188
#define TX39_IOMFIODATAIN       0x18C
#define TX39_IOMFIODATASEL      0x190
#define TX39_IOIOPOWERDWN       0x194
#define TX39_IOMFIOPOWERDWN     0x198

/*
 * MFIO pin 13 is the touch panel's pen-down sense. The ROM reads it four
 * ways and they agree it is a level, not a pulse:
 *
 *   0x83C2645C  returns (MFIODATAIN & 0x2000) != 0 as a boolean predicate
 *   0x83C2639C  samples it twice with dispatches in between and compares —
 *               a debounce, which only makes sense for a level
 *   0x83C26760  shifts it down by 13 and, only if set, dispatches the pen
 *               event (selector 0x49)
 *
 * IOMFIODATADIR is programmed to 0xF607D002 at reset, whose bit 13 is clear,
 * so the pin is an input.
 *
 * Until this was wired up, a touch reached the OS driver and stopped there:
 * the driver sampled the coordinates correctly and then 0x83C26760 read zero
 * and returned without dispatching anything.
 */
#define MFIO_PEN_DOWN           (1u << 13)

/* ---- Clock / power ---- */
#define TX39_CLOCKCTRL          0x1C0
#define TX39_POWERCTRL          0x1C4

/* POWERCTRL bits */
#define PWRCTRL_ONBUTN          0x80000000u
#define PWRCTRL_PWRINT          0x40000000u
#define PWRCTRL_PWROK           0x20000000u
#define PWRCTRL_STPTIMERVAL_SHIFT 12
#define PWRCTRL_STPTIMERVAL_MASK  0xFu
#define PWRCTRL_ENSTPTIMER      0x00000800u
#define PWRCTRL_FORCESHUTDWN    0x00000200u
#define PWRCTRL_WARMSTART       0x00000040u
#define PWRCTRL_STOPCPU         0x00000010u
#define PWRCTRL_COLDSTART       0x00000004u
#define PWRCTRL_PWRCS           0x00000002u
#define PWRCTRL_VCCON           0x00000001u

/* CLOCKCTRL bits */
#define CLK_ENCHIMCLK           0x00080000u
#define CLK_ENVIDCLK            0x00040000u
#define CLK_ENMBUSCLK           0x00020000u
#define CLK_ENSPICLK            0x00010000u
#define CLK_ENTIMERCLK          0x00008000u
#define CLK_ENFASTTIMERCLK      0x00004000u
#define CLK_ENSIBMCLK           0x00000800u
#define CLK_ENCSERCLK           0x00000008u

/*
 * Registers above POWERCTRL that the TX3912 documentation does not describe.
 * This part is a custom variant, and its idle power scan reads 0x1D8 and
 * 0x1F0 on every pass (from ROM 0x13C3A17C and 0x13C3A14C). They are not
 * written, and the OS proceeds with whatever they return, so nothing appears
 * to depend on them yet. See docs/OPEN_QUESTIONS.md.
 */
#define TX39_EXT_FIRST          0x1C8
#define TX39_EXT_LAST           0x1FC

#endif /* MH_TX39_REGS_H */
