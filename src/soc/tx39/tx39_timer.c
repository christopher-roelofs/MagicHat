/*
 * tx39_timer.c — TX39 RTC, alarm and periodic timer.
 *
 * All three run from the 32.768 kHz crystal, which on the DataRover is the
 * separate low-speed oscillator (see docs/HARDWARE.md). The RTC is a 40-bit
 * free-running counter: RTCHI holds bits 39..32, RTCLO bits 31..0.
 *
 * We derive the counter from the CPU cycle count rather than stepping it, so
 * the RTC stays correct regardless of how coarsely the caller ticks us.
 */
#include "soc/tx39/tx39.h"

uint64_t mh_timer_rtc_now(const tx39_timer *t)
{
    const tx39 *s = t->soc;
    uint64_t elapsed = s->cpu->cycle_count - t->rtc_cycle_ref;
    return t->rtc + (elapsed * TX39_RTCLOCK_HZ) / s->cpu_hz;
}

static void rtc_sync(tx39_timer *t)
{
    t->rtc = mh_timer_rtc_now(t);
    t->rtc_cycle_ref = t->soc->cpu->cycle_count;
}

uint32_t mh_timer_read(tx39_timer *t, uint32_t off, bool *decoded)
{
    *decoded = true;
    uint64_t now = mh_timer_rtc_now(t);

    switch (off) {
    case TX39_TIMERRTCHI:    return (uint32_t)((now >> 32) & 0x7FF);
    case TX39_TIMERRTCLO:    return (uint32_t)now;
    case TX39_TIMERALARMHI:  return t->alarm_hi;
    case TX39_TIMERALARMLO:  return t->alarm_lo;
    case TX39_TIMERCONTROL:  return t->control;
    case TX39_TIMERPERIODIC: return t->periodic;
    default:
        *decoded = false;
        return 0;
    }
}

bool mh_timer_write(tx39_timer *t, uint32_t off, uint32_t val)
{
    switch (off) {
    case TX39_TIMERALARMHI:  t->alarm_hi = val & 0x7FF; return true;
    case TX39_TIMERALARMLO:  t->alarm_lo = val; return true;
    case TX39_TIMERCONTROL:
        rtc_sync(t);
        if (val & TIMERCTRL_RTCCLR) {
            t->rtc = 0;
            t->rtc_cycle_ref = t->soc->cpu->cycle_count;
        }
        t->control = val;
        return true;
    case TX39_TIMERPERIODIC:
        t->periodic = val;
        t->periodic_reload = (val >> TIMERPER_PERVAL_SHIFT) & TIMERPER_MASK;
        return true;
    case TX39_TIMERRTCHI:
    case TX39_TIMERRTCLO:
        /* Read-only on hardware: the counter is cleared via TIMERCONTROL. */
        return true;
    default:
        return false;
    }
}

void mh_timer_tick(tx39_timer *t)
{
    if (t->control & TIMERCTRL_FREEZERTC)
        return;

    uint64_t now = mh_timer_rtc_now(t);

    /* Alarm: fires when the 43-bit RTC matches ALARMHI:ALARMLO. */
    uint64_t alarm = ((uint64_t)t->alarm_hi << 32) | t->alarm_lo;
    if (alarm && now >= alarm && !(t->soc->icu.status[4] & INT5_ALARMINT))
        mh_icu_raise(&t->soc->icu, 5, INT5_ALARMINT);

    /*
     * Periodic timer. PERVAL is the reload value in 32.768 kHz ticks, so the
     * interrupt rate is RTCLOCK / PERVAL. We derive the period index from the
     * RTC and fire once per index change, which keeps the rate correct no
     * matter how coarsely tick() is called.
     */
    if ((t->control & TIMERCTRL_ENPERTIMER) && t->periodic_reload) {
        uint64_t period = now / t->periodic_reload;
        if (period != t->last_period) {
            t->last_period = period;
            mh_icu_raise(&t->soc->icu, 5, INT5_PERINT);
        }
    } else {
        t->last_period = t->periodic_reload ? now / t->periodic_reload : 0;
    }
}
