#ifndef MH_CARD_IMAGE_H
#define MH_CARD_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int fd; uint8_t *data; uint32_t size;
#ifdef _WIN32
    void *file, *mapping;   /* HANDLEs; fd is unused there */
#endif
} mh_card_image;
#ifdef _WIN32
#define MH_CARD_IMAGE_CLOSED {-1, 0, 0, 0, 0}
#else
#define MH_CARD_IMAGE_CLOSED {-1, 0, 0}
#endif
/* Existing images retain their size; create_size applies only to a new file. */
bool mh_card_image_open(mh_card_image *image, const char *path, uint32_t create_size);
bool mh_card_image_flush(mh_card_image *image);
void mh_card_image_close(mh_card_image *image);
#endif
