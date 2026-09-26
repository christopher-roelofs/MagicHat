#include "soc/tx39/tx39.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

static uint32_t control(tx39 *soc)
{
    return mh_tx39_read(soc, TX39_MBUSCTRL, 4);
}

int main(void)
{
    r3900 cpu = {0};
    tx39 soc;
    mh_tx39_init(&soc, &cpu, 36864000);

    /* The guest's initial discovery check must see an empty bus, including
     * after loading an older snapshot with a zero control register. */
    CHECK(control(&soc) == MBUSCTRL_INPUT_HIGH);
    soc.mbus.reg[0] = 0x08a3;
    CHECK(control(&soc) == (MBUSCTRL_INPUT_HIGH | 0x08a3));

    /* Replay the boot command and the runtime stop/read-modify-write path.
     * Neither a command nor stopping the controller can drive the input. */
    mh_tx39_write(&soc, TX39_MBUSDATA, 4, 0x64080000);
    mh_tx39_write(&soc, TX39_MBUSCTRL, 4, 0x08a3);
    CHECK(control(&soc) == (MBUSCTRL_INPUT_HIGH | 0x08a3));
    mh_tx39_write(&soc, TX39_MBUSREG_F4, 4, 0x1234);
    mh_tx39_write(&soc, TX39_MBUSCTRL, 4, control(&soc) & ~1u);
    CHECK(control(&soc) == (MBUSCTRL_INPUT_HIGH | 0x08a2));
    mh_tx39_write(&soc, TX39_MBUSCTRL, 4, 0);
    CHECK(control(&soc) == MBUSCTRL_INPUT_HIGH);
    CHECK(mh_tx39_read(&soc, TX39_MBUSREG_F4, 4) == 0x1234);

    /* Completion is still immediate, and high input is not device detect
     * or received data. Check interrupts after acknowledgement as well. */
    mh_tx39_write(&soc, TX39_MBUSCTRL, 4, MBUSCTRL_BUSY | 0x08a3);
    CHECK(control(&soc) == (MBUSCTRL_INPUT_HIGH | 0x08a3));
    mh_tx39_write(&soc, TX39_INTRCLEAR(2), 4, INT2_MBUS_MASK);
    mh_mbus_update(&soc.mbus);
    CHECK((mh_tx39_read(&soc, TX39_INTRSTATUS(2), 4) & INT2_MBUS_MASK)
          == (INT2_MBUSTXBUFAVAIL | INT2_MBUSEMPTY));
    puts("MBUS empty-bus discovery and command status passed");
    return 0;
}
