#include "host/power.h"

#include <stdio.h>
#include <string.h>

#ifdef MRC_HAVE_SDL
#include <SDL2/SDL.h>
#endif

#ifdef _WIN32
#include <windows.h>
#endif

#if defined(MRC_HAVE_SDL)

/*
 * SDL knows about power on Linux, macOS, Windows, Android and iOS, which is
 * every host this is likely to run on, so prefer it when it is present.
 */
const char *mrc_host_power_source(void) { return "SDL"; }

bool mrc_host_power_read(mrc_host_power *out)
{
    int secs, pct;
    SDL_PowerState st = SDL_GetPowerInfo(&secs, &pct);

    memset(out, 0, sizeof(*out));
    out->percent = pct;

    switch (st) {
    case SDL_POWERSTATE_ON_BATTERY:
        out->has_battery = true;
        return true;
    case SDL_POWERSTATE_CHARGING:
    case SDL_POWERSTATE_CHARGED:
        out->has_battery = true;
        out->on_ac = true;
        return true;
    case SDL_POWERSTATE_NO_BATTERY:
        out->on_ac = true;
        out->percent = -1;
        return true;
    default:
        return false;                   /* SDL_POWERSTATE_UNKNOWN */
    }
}

#elif defined(_WIN32)

const char *mrc_host_power_source(void) { return "GetSystemPowerStatus"; }

bool mrc_host_power_read(mrc_host_power *out)
{
    SYSTEM_POWER_STATUS s;
    if (!GetSystemPowerStatus(&s))
        return false;

    memset(out, 0, sizeof(*out));
    out->has_battery = !(s.BatteryFlag & 128);   /* 128 = no system battery */
    out->on_ac = s.ACLineStatus == 1;
    out->percent = s.BatteryLifePercent == 255 ? -1 : s.BatteryLifePercent;
    return true;
}

#elif defined(__linux__)

const char *mrc_host_power_source(void) { return "/sys/class/power_supply"; }

static bool read_int(const char *path, int *v)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fscanf(f, "%d", v) == 1;
    fclose(f);
    return ok;
}

bool mrc_host_power_read(mrc_host_power *out)
{
    memset(out, 0, sizeof(*out));
    out->percent = -1;

    /*
     * Battery names are not fixed — BAT0 on most laptops, BAT1 on some,
     * "battery" on some ARM boards — so try the usual few rather than
     * enumerating, which would drag in dirent for very little.
     */
    static const char *const names[] = { "BAT0", "BAT1", "battery", "BATT" };
    char path[128];
    int pct;

    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        snprintf(path, sizeof(path),
                 "/sys/class/power_supply/%s/capacity", names[i]);
        if (read_int(path, &pct)) {
            out->has_battery = true;
            out->percent = pct;
            break;
        }
    }

    int online = 0;
    if (read_int("/sys/class/power_supply/AC/online", &online) ||
        read_int("/sys/class/power_supply/ACAD/online", &online) ||
        read_int("/sys/class/power_supply/ADP1/online", &online))
        out->on_ac = online != 0;
    else
        out->on_ac = !out->has_battery;

    return out->has_battery || out->on_ac;
}

#else

const char *mrc_host_power_source(void) { return "none"; }

bool mrc_host_power_read(mrc_host_power *out)
{
    memset(out, 0, sizeof(*out));
    out->percent = -1;
    return false;
}

#endif
