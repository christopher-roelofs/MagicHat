#include "soc/tx39/tx39.h"
#include <stdio.h>
#include <stdlib.h>

#define CHECK(x) do { if (!(x)) { fprintf(stderr, "line %d: %s\n", __LINE__, #x); exit(1); } } while (0)

int main(void)
{
    r3900 cpu = {0};
    tx39 soc;
    mrc_tx39_init(&soc, &cpu, 36864000);
    tx39_uart *u = &soc.uart[0];
    u->console = false;
    mrc_uart_write(u, TX39_UART_CTRL2, 11); /* 19200 baud, 8N1 */
    mrc_uart_write(u, TX39_UART_CTRL1, UART_CTRL1_ENUART);
    CHECK(mrc_uart_frame_cycles(u) == 19200);
    CHECK(soc.icu.status[1] & INT2_UARTATXINT);
    mrc_icu_write(&soc.icu, 0x104, 0xffffffff);
    mrc_uart_update(u);
    CHECK(!(soc.icu.status[1] & (INT2_UARTATXINT | INT2_UARTAEMPTYINT)));
    mrc_uart_write(u, TX39_UART_TXHOLD, 'a');
    mrc_icu_write(&soc.icu, 0x104, 0xffffffff);
    mrc_uart_write(u, TX39_UART_TXHOLD, 'b');
    bool decoded;
    CHECK(!(mrc_uart_read(u, TX39_UART_CTRL1, &decoded) & UART_CTRL1_EMPTY));
    cpu.cycle_count = 19199;
    mrc_uart_update(u);
    CHECK(soc.uart_timing[0].shift == 'a' && soc.uart_timing[0].holding);
    CHECK(!(soc.icu.status[1] & INT2_UARTATXINT));
    cpu.cycle_count++;
    mrc_uart_update(u);
    CHECK(soc.uart_timing[0].shift == 'b' && !soc.uart_timing[0].holding);
    CHECK(soc.icu.status[1] & INT2_UARTATXINT);
    mrc_icu_write(&soc.icu, 0x104, 0xffffffff);
    mrc_uart_update(u);
    CHECK(!(soc.icu.status[1] & INT2_UARTATXINT));
    cpu.cycle_count = 38400;
    CHECK(mrc_uart_read(u, TX39_UART_CTRL1, &decoded) & UART_CTRL1_EMPTY);
    CHECK(soc.icu.status[1] & INT2_UARTAEMPTYINT);
    puts("UART timing and interrupt acknowledgement passed");
    return 0;
}
