#include "devices/glacier/glacier.h"
#include <stdio.h>
#include <stdlib.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    glacier g;
    mrc_glacier_init(&g, "slot");
    CHECK(mrc_glacier_read(&g, GLACIER_STATUS, 2) == GLACIER_ST_CD_MASK);
    mrc_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    mrc_glacier_set_present(&g, true);
    CHECK(mrc_glacier_read(&g, GLACIER_NREG, 2) == 0xffffffffu);
    CHECK(g.unknown_reads == 1);
    CHECK(!(mrc_glacier_read(&g, GLACIER_STATUS, 2) & GLACIER_ST_CD_MASK));
    CHECK(!mrc_glacier_irq(&g));
    CHECK(mrc_glacier_read(&g, GLACIER_FALL_PENDING, 2) == GLACIER_ST_CD_MASK);
    mrc_glacier_write(&g, GLACIER_FALL_ENABLE, 2, GLACIER_ST_CD_MASK);
    CHECK(mrc_glacier_irq(&g)); /* a masked edge stays latched */
    mrc_glacier_write(&g, GLACIER_FALL_PENDING, 2, GLACIER_ST_CD1);
    CHECK(mrc_glacier_irq(&g));
    CHECK(mrc_glacier_read(&g, GLACIER_FALL_PENDING, 2) == GLACIER_ST_CD2);
    mrc_glacier_write(&g, GLACIER_FALL_PENDING, 2, GLACIER_ST_CD2);
    CHECK(!mrc_glacier_irq(&g));
    mrc_glacier_set_present(&g, true);
    CHECK(!mrc_glacier_irq(&g)); /* stable input is not another edge */
    mrc_glacier_write(&g, GLACIER_RISE_ENABLE, 2, GLACIER_ST_CD_MASK);
    mrc_glacier_set_present(&g, false);
    CHECK(mrc_glacier_irq(&g));
    CHECK(mrc_glacier_read(&g, GLACIER_STATUS, 2) & GLACIER_ST_CD_MASK);
    mrc_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    CHECK(!mrc_glacier_irq(&g));
    mrc_glacier_set_ready_irq(&g, true);
    mrc_glacier_write(&g, GLACIER_STATUS, 2, 0);
    CHECK(mrc_glacier_read(&g, GLACIER_STATUS, 2) & 8);
    mrc_glacier_write(&g, GLACIER_FALL_ENABLE, 2, 8);
    mrc_glacier_set_ready_irq(&g, false);
    CHECK(mrc_glacier_irq(&g));
    mrc_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    CHECK(!(mrc_glacier_read(&g, GLACIER_STATUS, 2) & 8));
    mrc_glacier_write(&g, GLACIER_FALL_PENDING, 2, 8);
    CHECK(!mrc_glacier_irq(&g));
    mrc_glacier_set_card_irq(&g, false);
    CHECK(mrc_glacier_read(&g, GLACIER_STATUS, 2) & 4);
    mrc_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    mrc_glacier_write(&g, GLACIER_FALL_ENABLE, 2, 4);
    mrc_glacier_set_card_irq(&g, true);
    CHECK(mrc_glacier_irq(&g));
    CHECK(!(mrc_glacier_read(&g, GLACIER_STATUS, 2) & 4));
    mrc_glacier_write(&g, GLACIER_FALL_PENDING, 2, 4);
    CHECK(!mrc_glacier_irq(&g));
    mrc_glacier_set_memory_inputs(&g, true, false, true);
    CHECK((mrc_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mrc_glacier_write(&g, GLACIER_STATUS, 2, 0xffff);
    CHECK((mrc_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mrc_glacier_write(&g, GLACIER_STATUS, 2, 0);
    CHECK((mrc_glacier_read(&g, GLACIER_STATUS, 2) & 14) == 6);
    mrc_glacier_write(&g, GLACIER_RISE_PENDING, 2, 0xffff);
    mrc_glacier_write(&g, GLACIER_FALL_PENDING, 2, 0xffff);
    mrc_glacier_set_memory_inputs(&g, true, true, false);
    CHECK(mrc_glacier_read(&g, GLACIER_RISE_PENDING, 2) == 8);
    CHECK(mrc_glacier_read(&g, GLACIER_FALL_PENDING, 2) == 2);
    puts("Glacier card pins, edges and acknowledgements passed");
    return 0;
}
