/*
 * ucb1100.c — Philips UCB1100BE audio / telecom codec and touchscreen
 * interface ("Betty" on the DataRover board).
 *
 * A SIB slave with sixteen 16-bit control registers, read and written by the
 * host through the TX39 SIB subframe registers. This file implements the
 * register file, the touchscreen, and the interrupt the codec raises back at
 * the host.
 *
 * Register numbering and bit assignments follow the UCB1x00 family, which
 * the UCB1100 is a member of and which is documented both by the UCB1300
 * datasheet (docs/hardware/ in the old tree) and by Linux's
 * drivers/mfd/ucb1x00.h.
 *
 * Validated against the machine: the IDT monitor's `touch_init` programs
 * eleven of these registers and reads every one back, and reports the
 * expected value for each.
 */
#include "soc/tx39/tx39.h"

/* Register numbers. */
enum {
    UCB_IO_DATA   = 0x00,
    UCB_IO_DIR    = 0x01,
    UCB_IE_RIS    = 0x02,   /* rising-edge interrupt enable  */
    UCB_IE_FAL    = 0x03,   /* falling-edge interrupt enable */
    UCB_IE_STATUS = 0x04,   /* read: status; write: clear    */
    UCB_TC_A      = 0x05,
    UCB_TC_B      = 0x06,
    UCB_AC_A      = 0x07,
    UCB_AC_B      = 0x08,
    UCB_TS_CR     = 0x09,   /* touchscreen control */
    UCB_ADC_CR    = 0x0A,
    UCB_ADC_DATA  = 0x0B,
    UCB_ID        = 0x0C,
    UCB_MODE      = 0x0D,
};

/* Interrupt sources (UCB_IE_RIS / UCB_IE_FAL / UCB_IE_STATUS). */
#define UCB_IO_PIN_MASK 0x03FFu   /* the ten general-purpose pins */

#define UCB_IE_ADC      (1u << 11)
#define UCB_IE_TSPX     (1u << 12)
#define UCB_IE_TSMX     (1u << 13)

/* UCB_TS_CR */
#define UCB_TS_TSMX_POW (1u << 0)
#define UCB_TS_TSPX_POW (1u << 1)
#define UCB_TS_TSMY_POW (1u << 2)
#define UCB_TS_TSPY_POW (1u << 3)
#define UCB_TS_TSMX_GND (1u << 4)
#define UCB_TS_TSPX_GND (1u << 5)
#define UCB_TS_TSMY_GND (1u << 6)
#define UCB_TS_TSPY_GND (1u << 7)
#define UCB_TS_MODE_MASK  (3u << 8)
#define UCB_TS_MODE_INT   (0u << 8)   /* idle; TSPX interrupts on touch */
#define UCB_TS_MODE_PRES  (1u << 8)   /* measuring pressure  */
#define UCB_TS_MODE_POS   (2u << 8)   /* measuring position  */
#define UCB_TS_BIAS_ENA   (1u << 11)

/* UCB_ADC_CR */
#define UCB_ADC_SYNC_ENA  (1u << 0)
/*
 * The input-select field is bits 4:2 — three bits, selecting one of eight
 * channels: the four touch plates and four auxiliary inputs.
 *
 * The ROM uses both halves. While sampling the panel it writes ADC_CR values
 * 0xD001, 0xD009, 0xD00D, 0xD005 (each repeated with bit 7 added to start the
 * conversion), whose input fields are 0, 2, 3, 1 — the four plates, each used
 * once. When idle it writes 0x501C and 0x509C, selecting input 7, and code at
 * ROM 0x83C24C84 tests for input 6. Those are auxiliary channels; see
 * aux_sample() below.
 */
#define UCB_ADC_INP_SHIFT 2
#define UCB_ADC_INP_MASK  7u
#define UCB_ADC_INP_TSPX  0u
#define UCB_ADC_INP_TSMX  1u
#define UCB_ADC_INP_TSPY  2u
#define UCB_ADC_INP_TSMY  3u
#define UCB_ADC_INP_AD0   4u
#define UCB_ADC_INP_AD1   5u
#define UCB_ADC_INP_AD2   6u
#define UCB_ADC_INP_AD3   7u
#define UCB_ADC_START     (1u << 7)
#define UCB_ADC_ENA       (1u << 15)

#define ADC_FULL_SCALE    0x3FFu

/*
 * Panel resistances, in units where the ADC's full scale is 1023.
 *
 * We have no panel datasheet, but the OS tells us the range it expects. Its
 * touch state machine calls a predicate at 0x83C665D0 which rejects a sample
 * set unless **all four** cross-driven readings are at or below a threshold
 * held at RAM[0xEB84] — which the running system sets to **500**, just under
 * half of full scale. It then sums the four as its pressure metric, and that
 * sum is position-independent: the four diagonals contribute x + (1-x) twice
 * and y + (1-y) twice, so only the contact resistance survives.
 *
 * So a real touch on the real panel reads comfortably under 500 everywhere,
 * which bounds the layer resistance against the bias resistance. These
 * values put a firm mid-panel touch near 310 and the worst corner near 465,
 * inside the threshold across the whole panel with room to spare.
 *
 * This is inferred from the one piece of evidence available, not measured.
 * If a future finding pins the real values down, these should change.
 */
#define PANEL_BIAS_R      1023u
#define PANEL_LAYER_R      400u   /* end to end across one layer */
#define PANEL_CONTACT_R     50u   /* through the pen contact     */

/* UCB_ADC_DATA */
#define UCB_ADC_DAT_VAL   (1u << 15)
#define UCB_ADC_DAT_SHIFT 5
#define UCB_ADC_DAT_MASK  0x3FFu

/*
 * Recompute the interrupt the codec presents to the host.
 *
 * In UCB_TS_MODE_INT the touchscreen sits biased and idle, and a pen touch
 * pulls TSPX down — which is the TSPX interrupt source. That is how software
 * learns about a touch without polling the ADC.
 */
static void ucb_refresh_irq(ucb1100 *u)
{
    uint16_t mode = u->reg[UCB_TS_CR] & UCB_TS_MODE_MASK;

    if (mode == UCB_TS_MODE_INT && u->pen_down)
        u->reg[UCB_IE_STATUS] |= UCB_IE_TSPX;

    /* An enabled source that is asserted drives the codec's IRQ pin. Rising
     * and falling enables are separate registers; either can select a
     * source. */
    uint16_t enabled = u->reg[UCB_IE_RIS] | u->reg[UCB_IE_FAL];
    u->irq_out = (u->reg[UCB_IE_STATUS] & enabled) != 0;
}

/*
 * Battery levels reported by default.
 *
 * AD2 is the main battery and AD3 the backup. Left at zero both read flat and
 * Magic Cap opens with "no main battery" and "backup battery out of power or
 * missing" — true of what we model, but not a useful machine to hand someone.
 *
 * What a fresh cell actually reads is not known; there is no panel or board
 * datasheet. What is known is where the OS's own boundaries sit. The backup
 * has a four-state ladder, measured by reading the dialog text at each level:
 * missing at 0, "completely out of power" through 275, "almost out of power"
 * through 325, and healthy from 340 — about a third of full scale. The main
 * battery is healthy from 650 upward; below roughly 600 the machine runs
 * slower and its exact boundary has not been pinned down.
 *
 * 900 sits comfortably above both, and short of the rail in case full scale
 * means something of its own. It is a choice inside a measured healthy band,
 * not a measurement — override it with --aux-adc.
 */
#define UCB_AUX_MAIN_BATTERY_DEFAULT    900u
#define UCB_AUX_BACKUP_BATTERY_DEFAULT  900u

void mh_ucb_init(ucb1100 *u)
{
    u->aux[2] = UCB_AUX_MAIN_BATTERY_DEFAULT;
    u->aux[3] = UCB_AUX_BACKUP_BATTERY_DEFAULT;
}

/*
 * The codec's general-purpose input pins.
 *
 * These are pins on the package, not machine state, so the level lives here
 * rather than in ucb1100 -- adding a field would change the struct's size and
 * invalidate every snapshot on disk. There is one codec.
 *
 * Held low by default, which is what the emulator has always presented.
 */
static uint16_t g_gpio_in;

void mh_ucb_set_gpio_in(ucb1100 *u, uint16_t level)
{
    uint16_t changed = (uint16_t)(g_gpio_in ^ level);

    g_gpio_in = level;

    /* A pin that moved raises its status bit if software asked for that
     * edge. Bits 0..9 are the I/O pins; the rest belong to other sources. */
    uint16_t edges = (uint16_t)(((level & u->reg[UCB_IE_RIS]) |
                                 (~level & u->reg[UCB_IE_FAL])) & changed);
    u->reg[UCB_IE_STATUS] |= (uint16_t)(edges & UCB_IO_PIN_MASK);
    ucb_refresh_irq(u);
}

void mh_ucb_set_pen(ucb1100 *u, bool down, uint16_t x, uint16_t y)
{
    u->pen_down = down;
    if (down) {
        u->pen_x = x & UCB_ADC_DAT_MASK;
        u->pen_y = y & UCB_ADC_DAT_MASK;
    }
    ucb_refresh_irq(u);
}

/*
 * What the converter sees on the selected pin.
 *
 * A four-wire resistive panel is two resistive layers that touch at one
 * point. To read a coordinate you put a voltage gradient across one layer
 * and measure the *other* layer, which floats to the potential at the
 * contact point. So the axis you are measuring is set by which pair of
 * plates is driven, in TS_CR — not by which pin the ADC is looking at.
 *
 * The ROM does exactly this, four times per sample, at 0x13C25CBC:
 *
 *   TS_CR 0x0982  TSPX_POW | TSPY_GND      ADC input TSPX   cross-driven
 *   TS_CR 0x0918  TSPY_POW | TSMX_GND      ADC input TSPY   cross-driven
 *   TS_CR 0x0A12  TSPX_POW | TSMX_GND      ADC input TSMY   -> X
 *   TS_CR 0x0A48  TSPY_POW | TSMY_GND      ADC input TSMX   -> Y
 *
 * The first two are its pressure measurements (TS_CR mode field 1) and the
 * last two its position measurements (mode field 2).
 */
/*
 * The four auxiliary ADC channels. On this board they carry board voltages —
 * the OS reads AD3 continuously while idle and tests AD2 at ROM 0x83C24C84,
 * and with all of them reading zero it reports every battery as flat or
 * missing. What each one is, and what scale it uses, is not yet established;
 * see docs/OPEN_QUESTIONS.md.
 */
static uint16_t aux_sample(const ucb1100 *u, unsigned input)
{
    return u->aux[input - UCB_ADC_INP_AD0];
}

static uint16_t adc_sample(const ucb1100 *u)
{
    uint16_t ts = u->reg[UCB_TS_CR];
    unsigned input = (u->reg[UCB_ADC_CR] >> UCB_ADC_INP_SHIFT) &
                     UCB_ADC_INP_MASK;

    if (input >= UCB_ADC_INP_AD0)
        return aux_sample(u, input);

    bool x_driven = (ts & UCB_TS_TSPX_POW) && (ts & UCB_TS_TSMX_GND);
    bool y_driven = (ts & UCB_TS_TSPY_POW) && (ts & UCB_TS_TSMY_GND);
    bool measuring_x_layer = (input == UCB_ADC_INP_TSPX ||
                              input == UCB_ADC_INP_TSMX);

    if (!u->pen_down) {
        /*
         * With the layers apart, the one that is not driven floats. We report
         * zero rather than a plausible coordinate, so that a reading taken
         * with no pen on the panel cannot be mistaken for a touch.
         */
        if ((x_driven && !measuring_x_layer) || (y_driven && measuring_x_layer))
            return 0;
    }

    if (x_driven && !measuring_x_layer)
        return u->pen_x;                    /* Y layer floats to the X coord */
    if (y_driven && measuring_x_layer)
        return u->pen_y;                    /* X layer floats to the Y coord */

    /* Measuring a pin on the layer that is being driven gives its rail. */
    if (x_driven && measuring_x_layer)
        return input == UCB_ADC_INP_TSPX ? ADC_FULL_SCALE : 0;
    if (y_driven && !measuring_x_layer)
        return input == UCB_ADC_INP_TSPY ? ADC_FULL_SCALE : 0;

    /*
     * Cross-driven: one plate of each layer, so the only current path runs
     * along one layer to the contact point, through the contact, and along
     * the other layer to the grounded plate. The UCB drives through a bias
     * resistor, so the voltage at the powered pin is a divider between that
     * and the path:
     *
     *     V = full_scale * Rpath / (Rbias + Rpath)
     *
     * and Rpath is the distance from the powered plate to the contact, plus
     * the distance from the contact to the grounded plate, plus the contact
     * resistance itself. This is what the ROM's four pressure measurements
     * are reading; it takes all four diagonals, which between them span the
     * panel symmetrically:
     *
     *     TSMX->TSMY, TSMY->TSPX, TSPX->TSPY, TSPY->TSMX
     *
     * Returning one constant for all four gives it four readings that cannot
     * be reconciled with any contact point. This at least varies with
     * position the way the panel does.
     *
     * The resistances are a model, not a measurement — we have no panel
     * datasheet. See docs/OPEN_QUESTIONS.md.
     */
    if (!u->pen_down) {
        /* Layers apart: no path at all, so the powered pin sits at the rail
         * and a grounded one at zero. */
        return (input == UCB_ADC_INP_TSPX && (ts & UCB_TS_TSPX_POW)) ||
               (input == UCB_ADC_INP_TSMX && (ts & UCB_TS_TSMX_POW)) ||
               (input == UCB_ADC_INP_TSPY && (ts & UCB_TS_TSPY_POW)) ||
               (input == UCB_ADC_INP_TSMY && (ts & UCB_TS_TSMY_POW))
               ? ADC_FULL_SCALE : 0;
    }

    {
        /* Distance along each layer from a given plate to the contact,
         * scaled from the 10-bit coordinate onto the layer's resistance. */
        unsigned x = u->pen_x * PANEL_LAYER_R / ADC_FULL_SCALE;
        unsigned y = u->pen_y * PANEL_LAYER_R / ADC_FULL_SCALE;
        unsigned d_tsmx = x, d_tspx = PANEL_LAYER_R - x;
        unsigned d_tsmy = y, d_tspy = PANEL_LAYER_R - y;
        unsigned path = PANEL_CONTACT_R;

        if (ts & UCB_TS_TSMX_POW) path += d_tsmx;
        if (ts & UCB_TS_TSPX_POW) path += d_tspx;
        if (ts & UCB_TS_TSMY_POW) path += d_tsmy;
        if (ts & UCB_TS_TSPY_POW) path += d_tspy;
        if (ts & UCB_TS_TSMX_GND) path += d_tsmx;
        if (ts & UCB_TS_TSPX_GND) path += d_tspx;
        if (ts & UCB_TS_TSMY_GND) path += d_tsmy;
        if (ts & UCB_TS_TSPY_GND) path += d_tspy;

        return (uint16_t)((unsigned)ADC_FULL_SCALE * path /
                          (PANEL_BIAS_R + path));
    }
}

uint16_t mh_ucb_read(ucb1100 *u, unsigned reg)
{
    reg &= 0xF;

    switch (reg) {
    case UCB_IO_DATA: {
        /* A pin configured as an input reads the line, not the latch. */
        uint16_t dir = u->reg[UCB_IO_DIR];
        return (uint16_t)(((u->reg[UCB_IO_DATA] & dir) |
                           (g_gpio_in & ~dir)) & UCB_IO_PIN_MASK);
    }
    case UCB_ADC_DATA: {
        if (!(u->reg[UCB_ADC_CR] & UCB_ADC_ENA))
            return 0;
        return (uint16_t)(UCB_ADC_DAT_VAL |
                          ((adc_sample(u) & UCB_ADC_DAT_MASK)
                           << UCB_ADC_DAT_SHIFT));
    }
    default:
        return u->reg[reg];
    }
}

void mh_ucb_write(ucb1100 *u, unsigned reg, uint16_t val)
{
    reg &= 0xF;

    switch (reg) {
    case UCB_IE_STATUS:
        u->reg[reg] &= (uint16_t)~val;      /* write-one-to-clear */
        ucb_refresh_irq(u);
        return;
    case UCB_ADC_DATA:
        return;                              /* read-only */
    case UCB_IE_RIS:
    case UCB_IE_FAL:
    case UCB_TS_CR:
        u->reg[reg] = val;
        ucb_refresh_irq(u);
        return;
    default:
        u->reg[reg] = val;
        return;
    }
}

void mh_panel_px_to_raw(unsigned px, unsigned py, uint16_t *rx, uint16_t *ry)
{
    if (px >= PANEL_SCREEN_W)
        px = PANEL_SCREEN_W - 1;
    if (py >= PANEL_SCREEN_H)
        py = PANEL_SCREEN_H - 1;

    unsigned span_x = PANEL_SCREEN_W - 1, span_y = PANEL_SCREEN_H - 1;
    *rx = (uint16_t)(PANEL_RAW_X0 +
                     (px * (PANEL_RAW_X1 - PANEL_RAW_X0) + span_x / 2) / span_x);
    *ry = (uint16_t)(PANEL_RAW_Y0 +
                     (py * (PANEL_RAW_Y1 - PANEL_RAW_Y0) + span_y / 2) / span_y);
}
