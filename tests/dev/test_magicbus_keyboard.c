#include "devices/magicbus/keyboard.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while (0)
int main(void)
{
    mrc_mb_keyboard k; mrc_mb_keyboard_init(&k);
    uint8_t out[260];
    /* Set-2 A press/release followed by extended Right press/release. */
    CHECK(mrc_mb_keyboard_key(&k,0x1c,false,true));
    CHECK(mrc_mb_keyboard_key(&k,0x1c,false,false));
    CHECK(mrc_mb_keyboard_key(&k,0x74,true,true));
    CHECK(mrc_mb_keyboard_key(&k,0x74,true,false));
    CHECK(mrc_mb_keyboard_read(&k,out,0)==0 && k.count==8);
    CHECK(mrc_mb_keyboard_read(&k,out,16)==9);
    const uint8_t expected[]={8,0x1c,0xf0,0x1c,0xe0,0x74,0xe0,0xf0,0x74};
    CHECK(!memcmp(out,expected,sizeof(expected)) && !k.count);
    CHECK(mrc_mb_keyboard_read(&k,out,16)==1 && out[0]==0);
    /* Queue wrap and transaction boundaries preserve the byte stream. */
    for(unsigned i=0;i<256;++i) CHECK(mrc_mb_keyboard_key(&k,0x1c,false,true));
    CHECK(!mrc_mb_keyboard_key(&k,0x74,true,false));
    CHECK(k.count==256 && k.dropped_events==1);
    unsigned total=0;
    while(k.count) {
        size_t n=mrc_mb_keyboard_read(&k,out,16);
        CHECK(n>1 && n<=16 && out[0]==n-1);
        for(size_t i=1;i<n;++i) CHECK(out[i]==0x1c);
        total+=out[0];
    }
    CHECK(total==256);
    CHECK(mrc_mb_keyboard_key(&k,0x12,false,true));
    const uint8_t led[]={'K',2,0xed,7,0,0,0,0};
    const uint8_t repeat[]={'K',3,0xf3,0x2b,0xf4,0,0,0};
    CHECK(mrc_mb_keyboard_write(&k,led,sizeof(led)) && k.leds==7);
    CHECK(mrc_mb_keyboard_write(&k,repeat,sizeof(repeat)) && k.repeat==0x2b);
    /* Truncated/unknown control packets are rejected without losing keys. */
    mrc_mb_keyboard before=k;
    for(size_t n=0;n<5;++n) CHECK(!mrc_mb_keyboard_write(&k,repeat,n));
    const uint8_t bad[]={'K',3,0xf3,0x2b,0xff};
    CHECK(!mrc_mb_keyboard_write(&k,bad,sizeof(bad)));
    CHECK(!memcmp(&k,&before,sizeof(k)));
    const uint8_t reset[]={'K',1,0xff,0,0,0,0,0};
    CHECK(mrc_mb_keyboard_write(&k,reset,sizeof(reset)));
    CHECK(!k.count && !k.leds && !k.repeat);
    CHECK(!mrc_mb_keyboard_key(&k,0xe0,false,true));
    puts("Magic Bus keyboard endpoint: packets, scan stream, bounds and reset passed");
}
