#include "host/card_image.h"
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
static bool valid_size(uint64_t n)
{ return n >= 65536 && n <= 64u*1024*1024 && !(n & (n-1)); }
bool mrc_card_image_open(mrc_card_image *i, const char *path, uint32_t create_size)
{
    *i = (mrc_card_image){.fd=-1};
    bool created=false;
    int fd=open(path,O_RDWR|O_CLOEXEC);
    if(fd<0 && errno==ENOENT) {
        if(!valid_size(create_size)) { fprintf(stderr,"card: invalid new image size\n"); return false; }
        fd=open(path,O_RDWR|O_CREAT|O_EXCL|O_CLOEXEC,0600); created=fd>=0;
    }
    if(fd<0) { fprintf(stderr,"card: %s: %s\n",path,strerror(errno)); return false; }
    if(flock(fd,LOCK_EX|LOCK_NB)<0) {
        fprintf(stderr,"card: %s is locked or cannot be locked\n",path); close(fd); return false;
    }
    struct stat st;
    if(fstat(fd,&st)<0 || !S_ISREG(st.st_mode) ||
       (created && ftruncate(fd,create_size)<0)) goto fail;
    uint64_t size=created?create_size:(uint64_t)st.st_size;
    if(!valid_size(size)) { fprintf(stderr,"card: image must be a power of two from 64 KiB to 64 MiB\n"); goto fail; }
    void *data=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
    if(data==MAP_FAILED) goto fail;
    *i=(mrc_card_image){.fd=fd,.data=data,.size=size};
    fprintf(stderr,"card: %s writable image %s (%u bytes)\n",created?"created":"opened",path,i->size);
    return true;
fail:
    fprintf(stderr,"card: cannot map %s\n",path); close(fd);
    if(created) unlink(path);
    return false;
}
bool mrc_card_image_flush(mrc_card_image *i)
{
    if(!i->data) return true;
    if(msync(i->data,i->size,MS_SYNC)<0 || fsync(i->fd)<0) {
        fprintf(stderr,"card: image flush failed: %s\n",strerror(errno)); return false;
    }
    return true;
}
void mrc_card_image_close(mrc_card_image *i)
{
    if(!i->data) return;
    mrc_card_image_flush(i);
    munmap(i->data,i->size); close(i->fd);
    *i=(mrc_card_image){.fd=-1};
}
