#ifndef MRC_CARD_IMAGE_H
#define MRC_CARD_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct {
    int fd; uint8_t *data; uint32_t size;
#ifdef _WIN32
    void *file, *mapping;   /* HANDLEs; fd is unused there */
#endif
} mrc_card_image;
#ifdef _WIN32
#define MRC_CARD_IMAGE_CLOSED {-1, 0, 0, 0, 0}
#else
#define MRC_CARD_IMAGE_CLOSED {-1, 0, 0}
#endif
/* Existing images retain their size; create_size applies only to a new file. */
bool mrc_card_image_open(mrc_card_image *image, const char *path, uint32_t create_size);
bool mrc_card_image_flush(mrc_card_image *image);
void mrc_card_image_close(mrc_card_image *image);
#endif
