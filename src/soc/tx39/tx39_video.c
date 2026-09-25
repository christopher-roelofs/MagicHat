/*
 * tx39_video.c — TX3912 LCD controller.
 *
 * A DMA-driven flat framebuffer: VIDEOCTRL3 holds the base address, and
 * VIDEOCTRL2 the geometry. There is no drawing engine — the CPU writes
 * pixels into RAM and this block scans them out.
 *
 * Note the controller DOES have interrupt sources: INTRSTATUS1 bits LCDINT
 * and DFINT. The previous project concluded there was no LCD interrupt on
 * this SoC; that conclusion was drawn without the register map and should be
 * re-tested. We do not raise them yet, because we have no evidence of the
 * conditions that trigger them, and inventing one would be exactly the kind
 * of guess this rewrite exists to avoid. See docs/OPEN_QUESTIONS.md.
 */
#include "soc/tx39/tx39.h"

static unsigned ctrl_index(uint32_t off)
{
    return (off - TX39_VIDEOCTRL1) / 4;
}

uint32_t mrc_video_read(tx39_video *v, uint32_t off, bool *decoded)
{
    unsigned i = ctrl_index(off);
    if (i >= 14) {
        *decoded = false;
        return 0;
    }
    *decoded = true;
    return v->ctrl[i];
}

bool mrc_video_write(tx39_video *v, uint32_t off, uint32_t val)
{
    unsigned i = ctrl_index(off);
    if (i >= 14)
        return false;
    v->ctrl[i] = val;
    return true;
}

bool mrc_video_geometry(const tx39_video *v, uint32_t *fb_pa, unsigned *w,
                        unsigned *h, unsigned *bpp)
{
    uint32_t c1 = v->ctrl[0], c2 = v->ctrl[1], c3 = v->ctrl[2];

    if (!(c1 & VID1_ENVID))
        return false;

    uint32_t bank    = (c3 >> VID3_VIDBANK_SHIFT)   & VID3_VIDBANK_MASK;
    uint32_t basehi  = (c3 >> VID3_VIDBASEHI_SHIFT) & VID3_VIDBASEHI_MASK;
    *fb_pa = (bank << 20) | (basehi << 4);

    /* HORZVAL counts groups of four pixels; LINEVAL counts lines. Both are
     * stored as (value - 1), per the TX3912 programming model. */
    uint32_t horz = (c2 >> VID2_HORZVAL_SHIFT) & VID2_HORZVAL_MASK;
    uint32_t line = (c2 >> VID2_LINEVAL_SHIFT) & VID2_LINEVAL_MASK;
    *w = (horz + 1) * 4;
    *h = line + 1;

    switch ((c1 >> VID1_BITSEL_SHIFT) & VID1_BITSEL_MASK) {
    case VID1_BITSEL_MONO: *bpp = 1; break;
    case VID1_BITSEL_2BPP: *bpp = 2; break;
    case VID1_BITSEL_4BPP: *bpp = 4; break;
    default:               *bpp = 8; break;
    }
    return true;
}
