#include "devices/glacier/glacier.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    glacier g;
    mh_glacier_init(&g, "slot");
    CHECK(mh_glacier_read(&g, GLACIER_STATUS, 2) == GLACIER_ST_CD_MASK);
    mh_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    mh_glacier_set_present(&g, true);
    CHECK(mh_glacier_read(&g, GLACIER_NREG, 2) == 0xffffffffu);
    CHECK(g.unknown_reads == 1);
    CHECK(!(mh_glacier_read(&g, GLACIER_STATUS, 2) & GLACIER_ST_CD_MASK));
    CHECK(!mh_glacier_irq(&g));
    CHECK(mh_glacier_read(&g, GLACIER_FALL_PENDING, 2) == GLACIER_ST_CD_MASK);
    mh_glacier_write(&g, GLACIER_FALL_ENABLE, 2, GLACIER_ST_CD_MASK);
    CHECK(mh_glacier_irq(&g)); /* a masked edge stays latched */
    mh_glacier_write(&g, GLACIER_FALL_PENDING, 2, GLACIER_ST_CD1);
    CHECK(mh_glacier_irq(&g));
    CHECK(mh_glacier_read(&g, GLACIER_FALL_PENDING, 2) == GLACIER_ST_CD2);
    mh_glacier_write(&g, GLACIER_FALL_PENDING, 2, GLACIER_ST_CD2);
    CHECK(!mh_glacier_irq(&g));
    mh_glacier_set_present(&g, true);
    CHECK(!mh_glacier_irq(&g)); /* stable input is not another edge */
    mh_glacier_write(&g, GLACIER_RISE_ENABLE, 2, GLACIER_ST_CD_MASK);
    mh_glacier_set_present(&g, false);
    CHECK(mh_glacier_irq(&g));
    CHECK(mh_glacier_read(&g, GLACIER_STATUS, 2) & GLACIER_ST_CD_MASK);
    mh_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    CHECK(!mh_glacier_irq(&g));
    mh_glacier_set_ready_irq(&g, true);
    mh_glacier_write(&g, GLACIER_STATUS, 2, 0);
    CHECK(mh_glacier_read(&g, GLACIER_STATUS, 2) & 8);
    mh_glacier_write(&g, GLACIER_FALL_ENABLE, 2, 8);
    mh_glacier_set_ready_irq(&g, false);
    CHECK(mh_glacier_irq(&g));
    mh_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    CHECK(!(mh_glacier_read(&g, GLACIER_STATUS, 2) & 8));
    mh_glacier_write(&g, GLACIER_FALL_PENDING, 2, 8);
    CHECK(!mh_glacier_irq(&g));
    mh_glacier_set_card_irq(&g, false);
    CHECK(mh_glacier_read(&g, GLACIER_STATUS, 2) & 4);
    mh_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    mh_glacier_write(&g, GLACIER_FALL_ENABLE, 2, 4);
    mh_glacier_set_card_irq(&g, true);
    CHECK(mh_glacier_irq(&g));
    CHECK(!(mh_glacier_read(&g, GLACIER_STATUS, 2) & 4));
    mh_glacier_write(&g, GLACIER_FALL_PENDING, 2, 4);
    CHECK(!mh_glacier_irq(&g));
    mh_glacier_set_memory_inputs(&g, true, false, true);
    CHECK((mh_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mh_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    CHECK((mh_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mh_glacier_write(&g, GLACIER_STATUS, 2, 0);
    CHECK((mh_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mh_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    mh_glacier_write(&g, GLACIER_FALL_PENDING, 2, 0xffff);
    mh_glacier_set_memory_inputs(&g, true, true, false);
    CHECK(mh_glacier_read(&g, GLACIER_RISE_PENDING, 2) == 8);
    CHECK(mh_glacier_read(&g, GLACIER_FALL_PENDING, 2) == 2);
    puts("Glacier card pins, edges and acknowledgements passed");
    return 0;
}
