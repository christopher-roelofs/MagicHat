/*
 * Map the host's charge onto AD2, the main battery.
 *
 * Magic Cap draws a battery gauge in its title bar, and that gauge tracks AD2
 * live — no reboot needed to see it move. Sweeping AD2 against it from a
 * booted Desk gives the travel directly:
 *
 *   <= 600   empty — the outline only
 *   650      a sliver
 *   700      about a third
 *   800      most of the way
 *   >= 900   full
 *
 * So the gauge only starts filling above 600 and is full by 900. Mapping the
 * host across a wider range than that wastes most of it below the point where
 * anything is drawn: at 300..900, a host at 50% still showed an empty gauge.
 *
 * The main battery also has a warning, contrary to an earlier note here that
 * said it did not: at 660 and below the OS offers to shut off screen
 * illumination, communication and sound to prolong life; at 690 and above it
 * does not. So the threshold is in (660, 690], and a host below about 20%
 * lands under it — which is roughly when a person would expect to be told. An earlier version of this mapped 0..100% onto 650..1000
 * instead — entirely inside the healthy band — so it read the host battery
 * correctly and then showed a full gauge whatever the answer was. The bound
 * came from a "low AD2 makes the machine slower" result that was an artefact
 * of a fixed-budget test harness, and should not have been used.
 *
 * One real effect does survive: booting with a low reading takes roughly
 * twice as many instructions to reach the boot screen. That matters only when
 * starting from cold; the usual way in is a snapshot that has already booted.
 * Pass --aux-adc to override.
 */
#define ADC_GAUGE_EMPTY   600u
#define ADC_GAUGE_FULL    900u



#include "machines/datarover840/power.h"
unsigned mrc_host_power_to_adc(int percent)
{
    if (percent < 0)
        return ADC_GAUGE_FULL;          /* unknown: show a good battery */
    if (percent > 100)
        percent = 100;

    return ADC_GAUGE_EMPTY +
           (unsigned)percent * (ADC_GAUGE_FULL - ADC_GAUGE_EMPTY) / 100u;
}
