/*
 * tx39_power.c — TX39 power controller.
 *
 * Mostly a register, plus one piece of real behaviour: the stop timer.
 *
 * Software sets POWERCTRL.ENSTPTIMER to start a settling delay and then
 * waits for INTRSTATUS5.STPTIMERINT. The monitor's touch_init does exactly
 * this at ROM 0x83C0289C before it will talk to the touchscreen:
 *
 *     INTRCLEAR5 |= STPTIMERINT
 *     POWERCTRL  |= ENSTPTIMER
 *     poll INTRSTATUS5 until STPTIMERINT sets
 *
 * Without the timer the wait never ends and `tin` hangs.
 */
#include "soc/tx39/tx39.h"

/*
 * The delay STPTIMERVAL selects is not documented in anything we have. It is
 * a power-rail settling delay, so software waits for it but never measures
 * it, which means the duration affects timing fidelity and nothing else. We
 * run it off the same 32.768 kHz clock as the RTC at (STPTIMERVAL + 1)
 * ticks — roughly 30 microseconds to half a millisecond. Recorded as a guess
 * in docs/OPEN_QUESTIONS.md.
 */
static uint64_t stptimer_ticks(uint32_t ctrl)
{
    uint32_t val = (ctrl >> PWRCTRL_STPTIMERVAL_SHIFT) &
                   PWRCTRL_STPTIMERVAL_MASK;
    return val + 1;
}

uint32_t mrc_power_read(tx39_power *p, uint32_t off, bool *decoded)
{
    if (off != TX39_POWERCTRL) {
        *decoded = false;
        return 0;
    }
    *decoded = true;

    /*
     * PWROK reports that the supply rails are good, which on a machine that
     * is evidently running they are. ONBUTN is supplied by the physical
     * button input; software writes cannot change that input.
     */
    return p->ctrl | PWRCTRL_PWROK | (p->warm ? PWRCTRL_WARMSTART : 0);
}

bool mrc_power_write(tx39_power *p, uint32_t off, uint32_t val)
{
    if (off != TX39_POWERCTRL)
        return false;

    p->warm = false;
    bool was_armed = (p->ctrl & PWRCTRL_ENSTPTIMER) != 0;
    uint32_t previous = p->ctrl;
    /* ONBUTN and PWRINT are input pins; software cannot write them. */
    p->ctrl = (val & ~(PWRCTRL_ONBUTN | PWRCTRL_PWRINT)) |
              (p->ctrl & (PWRCTRL_ONBUTN | PWRCTRL_PWRINT));
    if ((previous ^ p->ctrl) & (PWRCTRL_PWRCS | PWRCTRL_VCCON))
        fprintf(p->soc->log, "[power] outputs %02X -> %02X at PC %08X\n",
                previous & 3, p->ctrl & 3,
                p->soc->pc_hint ? *p->soc->pc_hint : 0);
    /* The DataRover's shutdown routine (ROM 13C3B180..13C3B248) clears
     * VCCON and PWRCS together. Model the resulting suspended core, not a
     * software reset: TMPR3912 brief pp. 13-14 keeps standby power and RAM
     * across ordinary off/on. General STOPCPU/doze timing is separate. */
    if ((previous & (PWRCTRL_PWRCS | PWRCTRL_VCCON)) &&
        !(p->ctrl & (PWRCTRL_PWRCS | PWRCTRL_VCCON)))
        p->soc->cpu->power_stopped = true;

    if ((val & PWRCTRL_ENSTPTIMER) && !was_armed) {
        p->stptimer_armed = true;
        p->stptimer_deadline = mrc_timer_rtc_now(&p->soc->timer) +
                               stptimer_ticks(val);
    } else if (!(val & PWRCTRL_ENSTPTIMER)) {
        p->stptimer_armed = false;
    }
    return true;
}

void mrc_power_tick(tx39_power *p)
{
    if (!p->stptimer_armed)
        return;
    if (mrc_timer_rtc_now(&p->soc->timer) < p->stptimer_deadline)
        return;

    p->stptimer_armed = false;
    mrc_icu_raise(&p->soc->icu, 5, INT5_STPTIMERINT);
}

void mrc_power_set_button(tx39_power *p, bool pressed)
{
    /* NetBSD TX39 power/ICU register definitions:
     * ONBUTN status and separate positive/negative button-edge sources. */
    bool old = (p->ctrl & PWRCTRL_ONBUTN) != 0;
    if (old == pressed) return;
    p->ctrl ^= PWRCTRL_ONBUTN;
    /* TMPR3912 product brief, p. 14: ONBUTN asserts PWRCS when PWROK
     * is high. Our modeled supply is good (mrc_power_read). This external
     * wake action is separate from the button's edge interrupt. */
    if (pressed) {
        p->ctrl |= PWRCTRL_PWRCS;
        p->soc->cpu->power_stopped = false;
    }
    mrc_icu_raise(&p->soc->icu, 5, pressed ? INT5_POSONBUTNINT : INT5_NEGONBUTNINT);
}
