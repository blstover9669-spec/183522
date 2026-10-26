/*
 * web_image.c - image decoders for the Web Browser.
 */
#include "web.h"

surface_t *web_image_decode(const uint8_t *data, uint32_t len) {
    if (!data || len < 4) return NULL;
    return image_decode(data, len);
}
