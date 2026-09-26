#include "host/card_image.h"
#include "devices/pccard/pccard.h"
#include "util/fs.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#define CHECK(x) do { if(!(x)) { fprintf(stderr,"line %d: %s\n",__LINE__,#x); exit(1); } } while(0)
int main(void)
{
    char path[512]; snprintf(path,sizeof(path),"%s/mh-sram-XXXXXX",mh_temp_dir());
    int fd=mkstemp(path); CHECK(fd>=0); close(fd); CHECK(unlink(path)==0);
    mh_card_image image,second;
    CHECK(mh_card_image_open(&image,path,2u*1024*1024));
    CHECK(image.size==2u*1024*1024 && image.data[0]==0);
    CHECK(!mh_card_image_open(&second,path,2u*1024*1024)); /* exclusive writer */
    mh_pccard c; uint32_t v;
    mh_pccard_sram_init(&c,1,image.data,image.size);
    CHECK(c.kind->read(&c,MH_PCCARD_WINDOW_A,0,1,&v) && v==1);
    CHECK(c.kind->read(&c,MH_PCCARD_WINDOW_A,4,1,&v) && v==0x61);
    CHECK(c.kind->read(&c,MH_PCCARD_WINDOW_A,6,1,&v) && v==0x7c);
    CHECK(c.kind->read(&c,MH_PCCARD_WINDOW_A,1,1,&v) && v==0xff);
    CHECK(!c.kind->write(&c,MH_PCCARD_WINDOW_A,0,1,0)); /* read-only CIS */
    CHECK(c.kind->write(&c,MH_PCCARD_WINDOW_B,image.size-2,4,0x12345678));
    CHECK(image.data[image.size-2]==0x12 && image.data[image.size-1]==0x34);
    CHECK(image.data[0]==0x56 && image.data[1]==0x78);
    CHECK(c.kind->read(&c,MH_PCCARD_WINDOW_B,image.size-2,4,&v) && v==0x12345678);
    CHECK(mh_card_image_flush(&image)); mh_card_image_close(&image);
    CHECK(mh_card_image_open(&image,path,1024*1024)); /* never resize existing files */
    CHECK(image.size==2u*1024*1024 && image.data[0]==0x56 && image.data[image.size-1]==0x34);
    mh_card_image_close(&image); mh_card_image_close(&image);
    CHECK(truncate(path,123)==0);
    CHECK(!mh_card_image_open(&image,path,2u*1024*1024));
    struct stat st; CHECK(stat(path,&st)==0 && st.st_size==123);
    CHECK(unlink(path)==0);
    puts("SRAM CIS, byte lanes, persistence, locking and size validation passed");
    return 0;
}
