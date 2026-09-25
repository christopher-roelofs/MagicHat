#ifndef MRC_CARD_IMAGE_H
#define MRC_CARD_IMAGE_H
#include <stdbool.h>
#include <stdint.h>
typedef struct { int fd; uint8_t *data; uint32_t size; } mrc_card_image;
/* Existing images retain their size; create_size applies only to a new file. */
bool mrc_card_image_open(mrc_card_image *image, const char *path, uint32_t create_size);
bool mrc_card_image_flush(mrc_card_image *image);
void mrc_card_image_close(mrc_card_image *image);
#endif
